/**
 * moonlight-sideband 主入口
 *
 * 集成的旁路服务端，模块化架构：
 *   - CursorModule:  光标捕获与广播（兼容原 windows-cursor-streamer 协议）
 *   - DisplayModule: 显示器控制（阶段 2 实现）
 *   - 未来模块...
 *
 * 兼容性：与原 CursorMonitor.exe 二进制兼容，
 *         现有 Android 客户端无需修改即可连接并接收光标。
 */

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellscalingapi.h>

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>

#include "Logger.hpp"
#include "SidebandServer.hpp"
#include "modules/CursorModule.hpp"
#include "modules/DisplayModule.hpp"

static std::atomic<SidebandServer *> g_server{nullptr};

static void OnSignal(int)
{
    SidebandServer *s = g_server.load();
    if (s)
        s->RequestStop();
}

static void ShowUsage()
{
    std::cout << "Usage: moonlight_sideband [options]\n"
              << "  -l LVL   Set log level (TRACE, DEBUG, INFO, ERROR)\n"
              << "  -p PORT  Set listen port (default 5005)\n"
              << "  -h       Show this help\n";
}

int main(int argc, char *argv[])
{
    LogLevel lvl = LogLevel::INFO;
    uint16_t port = 5005;

    for (int i = 1; i < argc; ++i)
    {
        std::string s = argv[i];
        if ((s == "-l" || s == "--log") && i + 1 < argc)
        {
            std::string v = argv[++i];
            if (v == "INFO") lvl = LogLevel::INFO;
            else if (v == "DEBUG") lvl = LogLevel::DEBUG;
            else if (v == "TRACE") lvl = LogLevel::TRACE;
            else if (v == "ERROR") lvl = LogLevel::LOG_ERROR;
        }
        else if ((s == "-p" || s == "--port") && i + 1 < argc)
        {
            port = (uint16_t)std::stoi(argv[++i]);
        }
        else if (s == "-h" || s == "--help")
        {
            ShowUsage();
            return 0;
        }
    }
    Logger::Get().SetLogLevel(lvl);

    // 强制开启高 DPI
    HMODULE hShcore = LoadLibraryA("Shcore.dll");
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

    Logger::Get().Info("======= moonlight-sideband 启动 =======");
    Logger::Get().Info("协议版本: 1, 端口: ", port);

    // 创建服务器
    SidebandServer server;
    if (!server.Initialize(port))
    {
        Logger::Get().Error("服务器初始化失败");
        return 1;
    }

    // 注册模块
    auto cursorMod = std::make_unique<CursorModule>(server);
    auto displayMod = std::make_unique<DisplayModule>(server);

    CursorModule *cursorPtr = cursorMod.get();
    DisplayModule *displayPtr = displayMod.get();

    server.RegisterModule(std::move(cursorMod));
    server.RegisterModule(std::move(displayMod));

    // 启动模块
    if (!cursorPtr->Start())
    {
        Logger::Get().Error("CursorModule 启动失败");
        return 1;
    }
    if (!displayPtr->Start())
    {
        Logger::Get().Error("DisplayModule 启动失败");
        // Display 是骨架，启动失败不致命
    }

    // 注册信号处理
    g_server.store(&server);
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 主循环（阻塞）
    server.Run();

    // 清理
    cursorPtr->Stop();
    displayPtr->Stop();
    server.Shutdown();
    g_server.store(nullptr);

    Logger::Get().Info("======= moonlight-sideband 退出 =======");
    return 0;
}
