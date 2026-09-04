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

    // 工作循环
    void WorkerLoop();
    void TextCursorMonitorLoop();
    void HookLoop();   // 钩子线程：安装钩子 + 消息循环
    void UpdateTextCursorState(bool forceUpdate = false);

    // 取系统插入符的屏幕位置。
    // 返回值：true = 拿到有效插入符；false = 当前无插入符可报。
    // outX/outY: 插入符底边中点的屏幕物理坐标（基准是 bottom ——
    //            下游"别让软键盘挡住输入行"要避开的是行底，而非行顶）。
    // outHeight: 插入符高度（像素，0 = 未知）。
    // outSource: 来源标记，SidebandProtocol::CARET_SOURCE_*。
    //
    // 阶段一仅实现 Win32 系统插入符（GUITHREADINFO）。Chromium/Electron、
    // UWP/WinUI、Qt、Flutter、Java 等自绘 caret 的框架 hwndCaret 为 NULL，
    // 此处返回 false —— 比返回一个冒充的位置更诚实。后续阶段在此追加
    // WinEvent OBJID_CARET / MSAA / UIA 路径（对应 CARET_SOURCE_* 扩充值）。
    bool GetCaretScreenPosition(int &outX, int &outY,
                                int &outHeight, int &outSource);

    // 供钩子回调调用：只置标志 + 唤醒，必须立即返回
    void PokeTextCursor(bool force);

    // 钩子回调（静态，转发到实例）
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id, LONG, DWORD, DWORD);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 单例指针（钩子回调转发用）
    static CursorModule *s_instance;
};
