#pragma once

#include "ISidebandModule.hpp"
#include "SidebandSession.hpp"
#include "SidebandServer.hpp"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <chrono>
#include <memory>

// user32.dll 未公开 API 定义
typedef BOOL(WINAPI *GETCURSORFRAMEINFO)(HCURSOR, DWORD, DWORD, DWORD *, DWORD *);

class SidebandServer;

// UIA 客户端主接口（定义在 <UIAutomationClient.h>，仅 CursorModule.cpp 引入）。
// 此处放在全局作用域做前向声明 —— 不能放进类里：类内声明会创建一个同名
// 嵌套类型（CursorModule::IUIAutomation）遮蔽全局版本，导致 .cpp 里所有
// UIA 调用全部解析到空类型（C2027/C2556/C2787 连锁报错的根因）。
// MIDL_INTERFACE 展开为 struct，前向声明也用 struct 才能与完整定义合并。
struct IUIAutomation;

/**
 * CursorEngine - 光标捕获与编码
 *
 * 从 windows-cursor-streamer 移植，主要改动：
 * - 持有 SidebandServer& 而非 NetworkManager&
 * - 调用 server.BroadcastCursor/BroadcastTextCursorState
 * - 服务端 PNG 缓存走 server.CachePng/GetCachedPng
 */
class CursorEngine
{
public:
    CursorEngine(SidebandServer &net);
    ~CursorEngine();

    void CaptureAndSend();

    // 显示配置变化（WM_DISPLAYCHANGE / 切换显示器）后的软重置：
    // 作废所有依赖旧显示器/旧 DPI 的缓存。只在捕获线程（WorkerLoop）调用 ——
    // 这些成员没有同步保护，跨线程直接调会与正在进行的捕获竞争。
    void ResetAfterDisplayChange();

private:
    ULONG_PTR m_token;
    SidebandServer &m_net;
    HMODULE m_hUser32;
    GETCURSORFRAMEINFO m_pGetCursorFrameInfo;

    // 状态缓存
    HCURSOR mLastCursor = NULL;
    int mLastTierSize = -1;
    // 光标大小设置缓存（注册表 CursorBaseSize，GetTargetSize 内部 2s 轮询）。
    // -1 = 未初始化。变化时强制重捕：Windows 原位重建共享光标的位图内容，
    // 句柄值不变，仅靠句柄比较检测不到尺寸变更。
    int mLastTargetSize = -1;
    std::chrono::steady_clock::time_point mLastProcessTime;

    // DPI 缓存
    HMONITOR mLastMonitor = NULL;
    UINT mLastDpi = 96;
    std::chrono::steady_clock::time_point mLastDpiCheckTime;

    // 防抖与重启
    std::chrono::steady_clock::time_point m_dpiChangeStartTime;
    bool m_isDpiChanging = false;

    // GDI 资源池
    HDC m_hMemDC = NULL;
    HBITMAP m_hBmpB = NULL;
    HBITMAP m_hBmpW = NULL;
    void *m_pBitsB = NULL;
    void *m_pBitsW = NULL;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;

    // 内存复用池
    std::vector<uint32_t> m_rawPixels;
    std::vector<uint8_t> m_xorMask;
    std::vector<uint8_t> m_pngBuffer;

    static int GetEncoderClsid(const WCHAR *format, CLSID *pClsid);
    bool RecreateResources(int w, int hTotal);
    void FreeResources();
    int GetTargetSize();
    UINT GetCursorMonitorDPI();
    int GetExpectedSystemCursorSize(int baseSize, UINT dpi);
    std::pair<int, int> GetAnimInfo(HCURSOR h);
};

/**
 * CursorModule - 旁路服务光标模块
 *
 * 职责：
 * 1. 在工作线程中捕获系统光标并广播给所有客户端（兼容老协议）
 * 2. 监控文本插入符位置，广播 Y 轴百分比
 * 3. 维护 WinEvent 钩子（光标变化）和低级鼠标/键盘钩子（文本插入符刷新）
 *
 * 与原 windows-cursor-streamer 的差异：
 * - 不再自己管理 socket，所有发送走 SidebandServer
 * - 客户端会话由 SidebandServer 管理，模块只关心业务
 */
class CursorModule : public ISidebandModule
{
public:
    explicit CursorModule(SidebandServer &server);
    ~CursorModule() override;

    const char *GetName() const override { return "Cursor"; }

    void OnClientConnected(SidebandSession &session) override;
    void OnClientDisconnected(SidebandSession &session) override;
    void OnDisplayChanged() override;

    // 生命周期（现在是 ISidebandModule 接口的一部分，由 SidebandServer 统一调用）
    bool Start() override;
    void Stop() override;

private:
    SidebandServer &m_server;
    std::unique_ptr<CursorEngine> m_engine;

    // 工作线程
    std::thread m_workerThread;
    std::thread m_textCursorThread;
    std::thread m_hookThread;   // 钩子消息循环线程（WinEvent + 低级鼠标/键盘钩子要求）
    std::atomic<bool> m_exit{false};
    DWORD m_hookThreadId = 0;   // 钩子线程 ID，用于 PostThreadMessage 通知退出

    // 钩子
    HWINEVENTHOOK m_hWinEventHook = NULL;
    HHOOK m_hMouseHook = NULL;
    HHOOK m_hKeyboardHook = NULL;

    // 光标变化同步
    std::condition_variable m_cvCursorChanged;
    std::mutex m_mutexCursor;
    bool m_cursorChanged = false;
    // 显示配置变化标志：OnClientConnected / OnDisplayChanged 只置标志，
    // 由 WorkerLoop 在捕获线程消费并执行软重置（避免跨线程直接操作 engine）。
    bool m_displayResetPending = false;

    // 文本光标状态
    // m_lastSentState 的取值含义：
    //   -2 = 尚未发送过任何状态（初始值）
    //   -1 = 最近一次上报"无插入符"（不活跃）
    //   0..10000 = 最近一次上报的 Y 百分比（YPercent 所指为插入符底边）
    std::atomic<int> m_lastSentState{-2};
    std::atomic<bool> m_textCursorActive{false};

    // 文本光标唤醒信号。
    // 低级钩子回调（WH_MOUSE_LL / WH_KEYBOARD_LL）跑在全系统每一个输入事件上，
    // 单次耗时超过 LowLevelHooksTimeout（默认 300ms）Windows 就会静默把钩子摘掉，
    // 表现为"用着用着文本光标追踪就失效了"，期间全系统输入还会发涩。
    // 所以钩子里只置标志并唤醒本条件变量，真正的取词工作交给 m_textCursorThread。
    // 顺带也消除了 UpdateTextCursorState 里那两个函数级 static 变量
    // 被三个线程同时读写的数据竞争。
    std::mutex m_mutexTextCursor;
    std::condition_variable m_cvTextCursor;
    bool m_textCursorPoke = false;       // 有输入事件，需要刷新
    bool m_textCursorPokeForce = false;  // 需要强制刷新（鼠标左键抬起）

    // === UI Automation（阶段二：自绘 caret 应用兜底） ===
    // 全部只在 m_textCursorThread 上创建/使用/释放 —— UIA 是跨进程 COM 调用，
    // 目标应用挂死时调用会同步阻塞（UIA 无逐调用超时），所以只允许待在
    // 这个专用线程上，钩子回调与其他工作线程绝不触碰。
    // IUIAutomation 是全局前向声明（文件顶部），完整定义只在 .cpp 里可见。
    IUIAutomation *m_pUia = nullptr;
    bool m_uiaBroken = false;          // COM/UIA 初始化已知失败（如组件缺失）
    bool m_uiaComInited = false;       // 本线程 CoInitializeEx 成功过，Stop 时需配对释放
    std::chrono::steady_clock::time_point m_uiaNextRetry{};  // 失败后的重试时间点

    // 工作循环
    void WorkerLoop();
    void TextCursorMonitorLoop();
    void HookLoop();   // 钩子线程：安装钩子 + 消息循环
    void UpdateTextCursorState(bool forceUpdate = false);

    // 取系统插入符（Win32 caret，GUITHREADINFO 路径）。
    // 返回值：true = 拿到有效插入符；false = 本路径不可用（不代表无插入符，
    // 调用方需继续尝试 UIA 路径）。
    // outX/outY: 插入符底边中点的屏幕物理坐标（基准是 bottom ——
    //            下游"别让软键盘挡住输入行"要避开的是行底，而非行顶）。
    // outHeight: 插入符高度（像素，0 = 未知）。
    bool GetCaretViaWin32(int &outX, int &outY, int &outHeight);

    // UIA 兜底：给自绘 caret 的应用（Chromium/Electron、UWP/WinUI、Qt、
    // Flutter、Java……）取插入符位置。
    // 返回 true = 拿到；false = UIA 不可用/目标不支持/无聚焦文本区域。
    // 只允许在 m_textCursorThread 上调用（跨进程 COM，可能阻塞）。
    // 坐标语义与 GetCaretViaWin32 一致：底边中点 + 高度。
    // UIA 路径同时对 PMv2 进程自动换算 DPI 虚拟化坐标，目标进程
    // DPI-unaware 也不会带缩放偏移。
    bool GetCaretViaUIA(int &outX, int &outY, int &outHeight);

    // UIA 惰性初始化 + 失败退避（60s 才重试一次），返回可用的
    // IUIAutomation*（不可用时 nullptr）。只允许在 m_textCursorThread
    // 上调用（CoInitializeEx 线程亲和）。
    IUIAutomation *EnsureUia();

    // 供钩子回调调用：只置标志 + 唤醒，必须立即返回
    void PokeTextCursor(bool force);

    // 钩子回调（静态，转发到实例）
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id, LONG, DWORD, DWORD);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 单例指针（钩子回调转发用）
    static CursorModule *s_instance;
};
