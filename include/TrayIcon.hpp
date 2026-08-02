#pragma once

/**
 * 托盘图标 + 隐藏主窗口
 *
 * 解决两件事：
 * 1. 进程原先没有任何退出路径 —— /SUBSYSTEM:WINDOWS 下没有控制台，
 *    main.cpp 里注册的 SIGINT/SIGTERM 处理器是死代码，只能 taskkill /F，
 *    模块的 Stop()、钩子卸载、Shutdown() 一律不执行。
 *    现在托盘右键就能优雅退出，注销时也会收到 WM_ENDSESSION。
 * 2. 拿到一个顶层窗口句柄后即可接收 WM_DISPLAYCHANGE 广播，
 *    DisplayModule 每 2 秒轮询的监控循环能被该事件提前唤醒（更快响应）。
 *
 * 注意：这里必须是普通顶层窗口而不是 message-only 窗口（HWND_MESSAGE），
 * 因为 message-only 窗口收不到 WM_DISPLAYCHANGE 这类广播消息。窗口创建后
 * 不调用 ShowWindow，因此用户看不到它。
 */

#include <windows.h>
#include <functional>
#include <string>

class TrayIcon
{
public:
    using Callback = std::function<void()>;

    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;

    // 创建隐藏窗口并添加托盘图标
    bool Create(const std::wstring &tooltip);

    // 移除托盘图标并销毁窗口
    void Destroy();

    // 用户点击"退出"或系统注销时触发
    void SetOnExit(Callback cb) { m_onExit = std::move(cb); }

    // 收到 WM_DISPLAYCHANGE 时触发（在 UI 线程调用，回调里不要做耗时操作）
    void SetOnDisplayChanged(Callback cb) { m_onDisplayChanged = std::move(cb); }

    // 阻塞运行消息循环，直到收到 WM_QUIT
    void RunMessageLoop();

    // 从其他线程请求退出消息循环
    void RequestQuit();

    // 更新托盘提示文字（例如显示当前客户端数）
    void SetTooltip(const std::wstring &tooltip);

    HWND GetHwnd() const { return m_hwnd; }

    // === 开机自启（HKCU\...\Run，不需要管理员权限）===
    static bool IsAutoStartEnabled();
    static bool SetAutoStart(bool enable);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowContextMenu();
    bool AddIcon();

    HWND m_hwnd = nullptr;
    HICON m_hIcon = nullptr;
    bool m_iconAdded = false;
    std::wstring m_tooltip;

    Callback m_onExit;
    Callback m_onDisplayChanged;

    // 资源管理器重启后需要重新添加图标
    UINT m_taskbarCreatedMsg = 0;
};
