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

    void ResetState();
    void CaptureAndSend();

private:
    ULONG_PTR m_token;
    SidebandServer &m_net;
    HMODULE m_hUser32;
    GETCURSORFRAMEINFO m_pGetCursorFrameInfo;

    // 状态缓存
    HCURSOR mLastCursor = NULL;
    int mLastTierSize = -1;
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

    // 启动/停止（由 main 调用，不是 ISidebandModule 接口的一部分）
    bool Start();
    void Stop();

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

    // 文本光标状态
    std::atomic<int> m_lastSentState{-2};
    std::atomic<bool> m_textCursorActive{false};

    // 工作循环
    void WorkerLoop();
    void TextCursorMonitorLoop();
    void HookLoop();   // 钩子线程：安装钩子 + 消息循环
    void UpdateTextCursorState(bool forceUpdate = false);
    bool GetCaretScreenPosition(int &outX, int &outY);

    // 钩子回调（静态，转发到实例）
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id, LONG, DWORD, DWORD);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 单例指针（钩子回调转发用）
    static CursorModule *s_instance;
};
