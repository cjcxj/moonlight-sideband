#include "TrayIcon.hpp"
#include "Logger.hpp"
#include "Win32Util.hpp"

#include <shellapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace
{

constexpr wchar_t kWindowClass[] = L"MoonlightSidebandHiddenWnd";
constexpr wchar_t kAutoRunKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoRunName[] = L"moonlight-sideband";

constexpr UINT WM_TRAY_NOTIFY = WM_APP + 1;
constexpr UINT kTrayIconId     = 1;

constexpr UINT ID_MENU_EXIT       = 40001;
constexpr UINT ID_MENU_AUTOSTART  = 40002;
constexpr UINT ID_MENU_OPENLOG    = 40003;
constexpr UINT ID_MENU_OPENCONFIG = 40004;

void OpenWithShell(const std::wstring &path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace

TrayIcon::~TrayIcon()
{
    Destroy();
}

bool TrayIcon::Create(const std::wstring &tooltip)
{
    m_tooltip = tooltip;

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWindowClass;

    // 类可能已注册（重复 Create），忽略 ERROR_CLASS_ALREADY_EXISTS
    if (!RegisterClassExW(&wc))
    {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
        {
            Logger::Get().Error("TrayIcon: RegisterClassEx 失败 err=", err);
            return false;
        }
    }

    // 普通顶层窗口（不显示）。message-only 窗口收不到 WM_DISPLAYCHANGE 广播。
    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,               // 不出现在 Alt+Tab / 任务栏
        kWindowClass, L"moonlight-sideband",
        WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
        nullptr, nullptr, hInst, this);

    if (!m_hwnd)
    {
        Logger::Get().Error("TrayIcon: CreateWindowEx 失败 err=", GetLastError());
        return false;
    }

    // 资源管理器崩溃/重启后托盘图标会丢失，需要重新添加
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    m_hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!AddIcon())
        Logger::Get().Warning("TrayIcon: 托盘图标添加失败（程序仍可运行）");

    Logger::Get().Info("TrayIcon: 隐藏窗口与托盘图标已创建");
    return true;
}

bool TrayIcon::AddIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_NOTIFY;
    nid.hIcon = m_hIcon;
    wcsncpy_s(nid.szTip, m_tooltip.c_str(), _TRUNCATE);

    m_iconAdded = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
    return m_iconAdded;
}

void TrayIcon::SetTooltip(const std::wstring &tooltip)
{
    m_tooltip = tooltip;
    if (!m_iconAdded || !m_hwnd)
        return;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, m_tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::Destroy()
{
    if (m_iconAdded && m_hwnd)
    {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_iconAdded = false;
    }

    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void TrayIcon::RequestQuit()
{
    if (m_hwnd)
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    else
        PostQuitMessage(0);
}

void TrayIcon::RunMessageLoop()
{
    MSG msg;
    BOOL r;
    while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0)
    {
        if (r == -1)
        {
            Logger::Get().Error("TrayIcon: GetMessage 返回 -1，退出消息循环");
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIcon::ShowContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, ID_MENU_OPENLOG,    L"打开日志");
    AppendMenuW(menu, MF_STRING, ID_MENU_OPENCONFIG, L"打开配置文件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_MENU_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);

    // 必须先 SetForegroundWindow，否则菜单在点击别处时不会消失（老牌 Win32 坑）
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TrayIcon *self = reinterpret_cast<TrayIcon *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (self && self->m_taskbarCreatedMsg != 0 && msg == self->m_taskbarCreatedMsg)
    {
        // 资源管理器重启，重新添加图标
        self->m_iconAdded = false;
        self->AddIcon();
        return 0;
    }

    switch (msg)
    {
    case WM_TRAY_NOTIFY:
        if (self && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP))
            self->ShowContextMenu();
        return 0;

    case WM_COMMAND:
        if (!self)
            break;
        switch (LOWORD(wParam))
        {
        case ID_MENU_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case ID_MENU_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            return 0;
        case ID_MENU_OPENLOG:
            OpenWithShell(Win32Util::GetExeDirectory() + L"moonlight_sideband.log");
            return 0;
        case ID_MENU_OPENCONFIG:
            OpenWithShell(Win32Util::GetExeDirectory() + L"moonlight_sideband.ini");
            return 0;
        default:
            break;
        }
        break;

    case WM_DISPLAYCHANGE:
        // 分辨率 / 显示器拓扑发生变化。回调里只置标志，实际枚举交给工作线程。
        if (self && self->m_onDisplayChanged)
            self->m_onDisplayChanged();
        return 0;

    case WM_QUERYENDSESSION:
        // 允许注销/关机
        return TRUE;

    case WM_ENDSESSION:
        if (wParam && self && self->m_onExit)
            self->m_onExit();
        return 0;

    case WM_CLOSE:
        if (self && self->m_onExit)
            self->m_onExit();
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
//                      开机自启
// ============================================================

bool TrayIcon::IsAutoStartEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    LSTATUS st = RegQueryValueExW(key, kAutoRunName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);

    return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool TrayIcon::SetAutoStart(bool enable)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAutoRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        Logger::Get().Error("TrayIcon: 打开 Run 注册表项失败");
        return false;
    }

    LSTATUS st;
    if (enable)
    {
        std::wstring cmd = L"\"" + Win32Util::GetExePath() + L"\"";
        st = RegSetValueExW(key, kAutoRunName, 0, REG_SZ,
                            reinterpret_cast<const BYTE *>(cmd.c_str()),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        st = RegDeleteValueW(key, kAutoRunName);
        if (st == ERROR_FILE_NOT_FOUND)
            st = ERROR_SUCCESS;  // 本来就没有，视为成功
    }
    RegCloseKey(key);

    if (st != ERROR_SUCCESS)
    {
        Logger::Get().Error("TrayIcon: 设置开机自启失败 st=", st);
        return false;
    }

    Logger::Get().Info("TrayIcon: 开机自启已", enable ? "开启" : "关闭");
    return true;
}
