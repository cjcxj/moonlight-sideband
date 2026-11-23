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

namespace Caret {
class CaretEngine;
}

/**
 * CursorModule - 旁路服务光标模块
 *
 * 职责：
 * 1. 在工作线程中捕获系统光标并广播给所有客户端（兼容老协议）
 * 2. 在专用 MTA 文本光标线程中通过 CaretEngine 多路探测（Win32 / Edit+Scintilla /
 *    MSAA / UIA / IMEAnchor / ControlBox）追踪输入焦点，并推送 CmdID=2 与 CmdID=102
 * 3. 在钩子线程中维护 WinEvent 钩子（光标/焦点/文本选区/IME）和低级鼠标/键盘钩子
 */
class CursorModule : public ISidebandModule
{
public:
    explicit CursorModule(SidebandServer &server,
                          bool allowImeFallback = true,
                          bool allowControlBoxForLegacy = false);
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
    bool m_allowImeFallback = true;
    bool m_allowControlBoxForLegacy = false;
    std::unique_ptr<CursorEngine> m_engine;
    std::unique_ptr<Caret::CaretEngine> m_caretEngine;

    // 工作线程
    std::thread m_workerThread;
    std::thread m_textCursorThread;
    std::thread m_hookThread;   // 钩子消息循环线程（WinEvent + 低级鼠标/键盘钩子要求）
    std::atomic<bool> m_exit{false};
    DWORD m_hookThreadId = 0;   // 钩子线程 ID，用于 PostThreadMessage 通知退出

    // 钩子
    HWINEVENTHOOK m_hWinEventHook = NULL;      // OBJID_CURSOR（鼠标光标形状变化）
    HWINEVENTHOOK m_hCaretFgHook = NULL;       // EVENT_SYSTEM_FOREGROUND
    HWINEVENTHOOK m_hCaretObjHook = NULL;      // EVENT_OBJECT_SHOW .. LOCATIONCHANGE
    HWINEVENTHOOK m_hCaretTextHook = NULL;     // TEXTSELECTIONCHANGED + IME_*
    HHOOK m_hMouseHook = NULL;
    HHOOK m_hKeyboardHook = NULL;

    // 光标变化同步
    std::condition_variable m_cvCursorChanged;
    std::mutex m_mutexCursor;
    bool m_cursorChanged = false;
    // 显示配置变化标志：OnClientConnected / OnDisplayChanged 只置标志，
    // 由 WorkerLoop 在捕获线程消费并执行软重置（避免跨线程直接操作 engine）。
    bool m_displayResetPending = false;

    // 文本光标活跃标志（供 KeyboardProc 快速判断是否需要积极唤醒）
    std::atomic<bool> m_textCursorActive{false};
    std::atomic<HWND> m_trackedFgHwnd{nullptr};
    std::atomic<HWND> m_trackedFocusHwnd{nullptr};

    // 文本光标唤醒信号与事件提示
    std::mutex m_mutexTextCursor;
    std::condition_variable m_cvTextCursor;
    bool m_textCursorPoke = false;
    bool m_textCursorPokeForce = false;
    bool m_textCursorResetPending = false;
    bool m_pendingCaretEvent = false;
    bool m_pendingImeEvent = false;
    HWND m_lastCaretEventHwnd = nullptr;
    LONG m_lastCaretEventObj = 0;
    LONG m_lastCaretEventChild = 0;
    bool m_imeVisible = false;
    HWND m_imeHwnd = nullptr;
    uint64_t m_lastImeEventMs = 0;
    uint64_t m_lastKeyboardInputMs = 0;
    std::atomic<uint64_t> m_lastKeyboardAtomicMs{0};
    std::atomic<uint64_t> m_lastInactiveKbWakeMs{0};

    // 工作循环
    void WorkerLoop();
    void TextCursorMonitorLoop();
    void HookLoop();   // 钩子线程：安装钩子 + 消息循环

    // 供钩子回调调用：只置标志 + 唤醒，必须立即返回
    void PokeTextCursor(bool force, bool fromKeyboard = false);
    void OnCaretWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild);

    // 钩子回调（静态，转发到实例）
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id, LONG, DWORD, DWORD);
    static void CALLBACK CaretWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                           LONG idObject, LONG idChild, DWORD, DWORD);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 单例指针（钩子回调转发用）
    static CursorModule *s_instance;
};
