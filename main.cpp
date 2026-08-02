/**
 * moonlight-sideband 主入口
 *
 * 集成的旁路服务端，模块化架构：
 *   - CursorModule:  光标捕获与广播（兼容原 windows-cursor-streamer 协议）
 *   - DisplayModule: 显示器控制
 *   - 未来模块...
 *
 * 兼容性：与原 CursorMonitor.exe 二进制兼容，
 *         现有 Android 客户端无需修改即可连接并接收光标。
 *
 * 进程形态：跟随用户会话的常驻进程（不是 Windows 服务）。
 * 这一点是刻意的 —— Windows 服务自 Vista 起运行在 Session 0，与交互桌面隔离，
 * 在那里 GetCursorInfo / 全局钩子抓不到用户桌面，ChangeDisplaySettingsEx /
 * SetDisplayConfig 作用的也是 Session 0 的桌面而非用户实际在用的桌面，
 * 本程序的两个功能都会失效。要开机自启请用托盘菜单里的"开机自启"
 *（写 HKCU\\...\\Run），或任务计划程序建一个"登录时触发"的任务。
 *
 * 线程布局：
 *   主线程        —— Win32 消息循环（托盘菜单 + WM_DISPLAYCHANGE）
 *   服务器线程    —— SidebandServer::Run()，WSAPoll 收发
 *   CursorModule  —— 工作线程 / 文本光标线程 / 钩子消息循环线程
 *   DisplayModule —— 监控线程（每 2 秒轮询 + hash 比较，事件可提前唤醒）
 */

// NOMINMAX / WIN32_LEAN_AND_MEAN / _WIN32_WINNT 等
// 由 CMakeLists.txt 的 add_compile_definitions 全局定义

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellscalingapi.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <exception>    // std::set_terminate / std::rethrow_exception
#include <cstdlib>      // std::abort
#include <iomanip>      // std::hex

#include "Logger.hpp"
#include "Config.hpp"
#include "TrayIcon.hpp"
#include "Win32Util.hpp"
#include "SidebandServer.hpp"
#include "modules/CursorModule.hpp"
#include "modules/DisplayModule.hpp"

namespace
{

// 单实例互斥体。名字用 Local\ 前缀 —— 每个登录会话允许一份，
// 这正是我们要的（本程序按用户会话运行）。
constexpr wchar_t kInstanceMutexName[] = L"Local\\moonlight-sideband-single-instance";

// 未处理异常捕获：记录崩溃信息到日志
LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
    Logger::Get().Error("======= 程序崩溃 =======");
    Logger::Get().Error("异常代码: 0x", std::hex, ep->ExceptionRecord->ExceptionCode);
    Logger::Get().Error("地址: 0x", std::hex,
                        (uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) // ACCESS_VIOLATION
    {
        Logger::Get().Error("访问违规, 类型: ",
                            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "读" : "写",
                            " 地址: 0x", std::hex,
                            ep->ExceptionRecord->ExceptionInformation[1]);
    }
    Logger::Get().Error("=========================");
    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate 处理器：C++ 异常未捕获时触发
void TerminateHandler()
{
    Logger::Get().Error("======= std::terminate 被调用 =======");
    try
    {
        auto ex = std::current_exception();
        if (ex)
            std::rethrow_exception(ex);
    }
    catch (const std::exception &e)
    {
        Logger::Get().Error("未捕获异常: ", e.what());
    }
    catch (...)
    {
        Logger::Get().Error("未捕获未知异常");
    }
    Logger::Get().Error("====================================");
    std::abort();
}

void EnablePerMonitorDpi()
{
    HMODULE hShcore = LoadLibraryW(L"Shcore.dll");
    if (hShcore)
    {
        typedef HRESULT(WINAPI * SDPA)(PROCESS_DPI_AWARENESS);
        SDPA p = (SDPA)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (p)
            p(PROCESS_PER_MONITOR_DPI_AWARE);
        FreeLibrary(hShcore);
    }
    else
    {
        SetProcessDPIAware();
    }
}

// 命令行仍然支持覆盖配置文件（方便临时调试）。
// 注意进程是 /SUBSYSTEM:WINDOWS，没有控制台，因此不再往 stdout 打印帮助 ——
// 解析结果只写日志。
void ApplyCommandLineOverrides(Config &cfg)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring s = argv[i];
        if ((s == L"-l" || s == L"--log") && i + 1 < argc)
        {
            cfg.logLevel = Win32Util::WideToUtf8(argv[++i]);
        }
        else if ((s == L"-p" || s == L"--port") && i + 1 < argc)
        {
            int p = _wtoi(argv[++i]);
            if (p > 0 && p <= 65535)
                cfg.port = (uint16_t)p;
        }
        else if (s == L"--loopback")
        {
            cfg.loopbackOnly = true;
        }
    }

    LocalFree(argv);
}

} // namespace

int main(int, char *[])
{
    // === 单实例检查 ===
    // 之前没有这道关：起第二份实例时，由于监听 socket 设了 SO_REUSEADDR，
    // 在 Windows 上第二次 bind 反而可能成功，两个进程抢同一个端口，行为未定义。
    HANDLE hInstanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (!hInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Logger::Get().Info("已有一份 moonlight-sideband 正在运行，本次启动退出");
        if (hInstanceMutex)
            CloseHandle(hInstanceMutex);
        return 0;
    }

    // === 配置 ===
    Config cfg;
    if (!cfg.LoadOrCreate())
        Logger::Get().Error("配置文件读写失败，使用默认配置继续");
    ApplyCommandLineOverrides(cfg);

    LogLevel lvl = LogLevel::INFO;
    Logger::ParseLevel(cfg.logLevel, lvl);
    Logger::Get().Init(lvl, cfg.logMaxBytes);

    Logger::Get().Info("======= moonlight-sideband 启动 =======");
    Logger::Get().Info("构建版本: ", SIDEBAND_BUILD_COMMIT, " (", __DATE__, " ", __TIME__, ")");
    Logger::Get().Info("协议版本: 1, 端口: ", cfg.port,
                       ", 监听: ", (cfg.loopbackOnly ? "仅本机" : "全部网卡"));
    Logger::Get().Info("配置文件: ", Win32Util::WideToUtf8(Config::FilePath()));

    EnablePerMonitorDpi();

    SetUnhandledExceptionFilter(CrashHandler);
    std::set_terminate(TerminateHandler);

    // === 服务器 ===
    SidebandServer server;
    if (!server.Initialize(cfg.port, cfg.loopbackOnly))
    {
        Logger::Get().Error("服务器初始化失败");
        CloseHandle(hInstanceMutex);
        return 1;
    }
    server.SetAuthToken(cfg.token);

    // 注册模块 —— Start/Stop 已在 ISidebandModule 接口里，
    // 这里不再需要持有具体类型的指针手动调用
    server.RegisterModule(std::make_unique<CursorModule>(server));
    server.RegisterModule(std::make_unique<DisplayModule>(server));
    server.StartModules();

    // === 托盘图标 + 隐藏窗口（主线程跑消息循环）===
    TrayIcon tray;
    std::atomic<bool> stopping{false};

    tray.SetOnExit([&]() {
        if (!stopping.exchange(true))
        {
            Logger::Get().Info("收到退出请求");
            server.RequestStop();
        }
    });

    // WM_DISPLAYCHANGE：转发给模块（模块只置标志唤醒自己的线程）
    tray.SetOnDisplayChanged([&]() { server.NotifyDisplayChanged(); });

    if (!tray.Create(L"moonlight-sideband"))
    {
        Logger::Get().Error("托盘/窗口创建失败，程序将以无 UI 方式运行（只能用任务管理器结束）");
    }

    // 服务器主循环放到后台线程，主线程留给 Win32 消息循环 ——
    // WM_DISPLAYCHANGE 与托盘菜单都必须在有消息泵的线程上处理
    std::thread serverThread([&server]() {
        try
        {
            server.Run();
        }
        catch (const std::exception &e)
        {
            Logger::Get().Error("SidebandServer::Run 异常: ", e.what());
        }
        catch (...)
        {
            Logger::Get().Error("SidebandServer::Run 未知异常");
        }
    });

    if (tray.GetHwnd())
    {
        tray.RunMessageLoop();
    }
    else
    {
        // 没有窗口时退化为等待服务器线程自然结束
        serverThread.join();
    }

    // === 清理 ===
    Logger::Get().Info("正在停止...");
    server.RequestStop();
    if (serverThread.joinable())
        serverThread.join();

    server.StopModules();
    server.Shutdown();
    tray.Destroy();

    Logger::Get().Info("======= moonlight-sideband 退出 =======");

    ReleaseMutex(hInstanceMutex);
    CloseHandle(hInstanceMutex);
    return 0;
}
