/**
 * DisplayModule 实现
 *
 * 实现：
 * - EnumDisplayDevicesW 枚举显示器
 * - EnumDisplaySettingsExW 获取分辨率/刷新率
 * - GetDpiForMonitor 获取缩放
 * - ChangeDisplaySettingsExW 切换主显示器
 * - 周期性监控主显示器变化
 */

#include "DisplayModule.hpp"
#include "Logger.hpp"
#include "SidebandProtocol.hpp"

#include <windows.h>
#include <shellscalingapi.h>

#include <sstream>
#include <chrono>
#include <cstdio>
#include <map>
#include <cstring>

#pragma comment(lib, "shcore.lib")  // GetDpiForMonitor
#pragma comment(lib, "user32.lib")

// ============================================================
//                      辅助函数
// ============================================================

std::string WideToUtf8(const std::wstring &w)
{
    if (w.empty())
        return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &out[0], len, nullptr, nullptr);
    return out;
}

std::string EscapeJson(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

// 从 JSON 字符串中提取字符串字段值（支持转义反转义）
std::string ParseJsonStringField(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return "";
    size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";

    // 查找未转义的结束引号
    size_t q2 = q1 + 1;
    while (q2 < json.size())
    {
        if (json[q2] == '\\' && q2 + 1 < json.size())
            q2 += 2;  // 跳过转义序列
        else if (json[q2] == '"')
            break;
        else
            ++q2;
    }
    if (q2 >= json.size()) return "";

    // 提取并反转义 JSON 转义序列
    std::string raw = json.substr(q1 + 1, q2 - q1 - 1);
    std::string result;
    result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\\' && i + 1 < raw.size())
        {
            switch (raw[i + 1])
            {
            case '"': result += '"';  ++i; break;
            case '\\': result += '\\'; ++i; break;
            case '/': result += '/';  ++i; break;
            case 'n': result += '\n'; ++i; break;
            case 't': result += '\t'; ++i; break;
            case 'r': result += '\r'; ++i; break;
            case 'b': result += '\b'; ++i; break;
            case 'f': result += '\f'; ++i; break;
            default: result += raw[i]; break;  // 未知转义，保留原字符
            }
        }
        else
        {
            result += raw[i];
        }
    }
    return result;
}

// 从 JSON 字符串中提取整数字段值（简易解析）
int ParseJsonIntField(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return 0;
    size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return 0;
    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size()) return 0;
    bool negative = false;
    if (json[start] == '-') { negative = true; ++start; }
    int value = 0;
    bool anyDigit = false;
    while (start < json.size() && json[start] >= '0' && json[start] <= '9')
    {
        value = value * 10 + (json[start] - '0');
        ++start;
        anyDigit = true;
    }
    if (!anyDigit) return 0;
    return negative ? -value : value;
}

// ============================================================
//                      DisplayModule
// ============================================================

DisplayModule::DisplayModule(SidebandServer &server) : m_server(server) {}

DisplayModule::~DisplayModule()
{
    Stop();
}

bool DisplayModule::Start()
{
    m_exit = false;
    m_monitorThread = std::thread([this]()
                                  { MonitorLoop(); });
    Logger::Get().Info("DisplayModule: 已启动 (含显示器枚举与切换)");
    return true;
}

void DisplayModule::Stop()
{
    m_exit = true;
    if (m_monitorThread.joinable())
        m_monitorThread.join();
}

void DisplayModule::OnClientConnected(SidebandSession &session)
{
    // 不在主线程中调用 EnumerateDisplays（会阻塞 ~50ms），
    // 设置标志位让 MonitorLoop 异步推送
    m_forcePush.store(true);
    Logger::Get().Debug("DisplayModule: 新客户端连接，已请求推送当前显示器状态");
}

// ============================================================
//                      枚举显示器
// ============================================================

// 用 CCD API 构建 GDI 设备名 → 显示器友好名称 的映射
// 例如 "\\.\DISPLAY1" → "Dell U2720Q"
static std::map<std::wstring, std::wstring> BuildFriendlyNameMap()
{
    std::map<std::wstring, std::wstring> result;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
        return result;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);

    if (QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return result;

    for (const auto &path : paths)
    {
        // 获取源设备名（GDI 设备名，如 "\\.\DISPLAY1"）
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        // 获取目标设备名（显示器友好名称，如 "Dell U2720Q"）
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS)
            continue;

        if (targetName.monitorFriendlyDeviceName[0] != L'\0')
        {
            result[sourceName.viewGdiDeviceName] = targetName.monitorFriendlyDeviceName;
        }
    }

    return result;
}

// EnumDisplayMonitors 回调上下文
struct EnumMonitorContext
{
    std::vector<DisplayModule::DisplayInfo> *result;
};

// EnumDisplayMonitors 回调：枚举当前桌面中所有可见的物理监视器
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdc, LPRECT lprcMonitor, LPARAM dwData)
{
    auto *ctx = reinterpret_cast<EnumMonitorContext *>(dwData);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi))
        return TRUE;  // 继续枚举下一个

    DisplayModule::DisplayInfo info;
    info.id = WideToUtf8(mi.szDevice);       // "\\.\DISPLAY1"
    info.isActive = true;                     // EnumDisplayMonitors 只返回活跃监视器
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.x = mi.rcMonitor.left;
    info.y = mi.rcMonitor.top;
    info.width = mi.rcMonitor.right - mi.rcMonitor.left;
    info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // 获取分辨率/刷新率/色深
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0))
    {
        info.width = (int)dm.dmPelsWidth;
        info.height = (int)dm.dmPelsHeight;
        info.refreshRate = (int)dm.dmDisplayFrequency;
        info.bitsPerPel = (int)dm.dmBitsPerPel;
    }

    // 获取监视器名称和 DeviceID（用于 PerMonitorSettings 注册表）
    DISPLAY_DEVICEW monitor = {};
    monitor.cb = sizeof(monitor);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &monitor, 0))
    {
        info.name = WideToUtf8(monitor.DeviceString);
        info.deviceId = WideToUtf8(monitor.DeviceID);
    }
    if (info.name.empty())
        info.name = "Display " + std::to_string(ctx->result->size() + 1);

    // 获取适配器名称
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
    {
        if (_wcsicmp(adapter.DeviceName, mi.szDevice) == 0)
        {
            info.adapterName = WideToUtf8(adapter.DeviceString);
            break;
        }
    }

    // 获取 DPI（缩放）
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        info.scale = (int)((dpiX * 100 + 48) / 96);
    }

    Logger::Get().Debug("DisplayModule: 监视器 ", info.id,
                        " ", info.width, "x", info.height, "@", info.refreshRate,
                        " scale=", info.scale, "% primary=", info.isPrimary);

    ctx->result->push_back(info);
    return TRUE;
}

std::vector<DisplayModule::DisplayInfo> DisplayModule::EnumerateDisplays() const
{
    std::vector<DisplayInfo> result;
    EnumMonitorContext ctx{&result};
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));

    // 用 CCD API 补充显示器友好名称（如 "Dell U2720Q"）
    auto nameMap = BuildFriendlyNameMap();
    for (auto &info : result)
    {
        std::wstring devName(info.id.begin(), info.id.end());
        auto it = nameMap.find(devName);
        if (it != nameMap.end() && it->second[0] != L'\0')
        {
            std::string friendly = WideToUtf8(it->second);
            if (!friendly.empty())
                info.name = friendly;
        }
    }

    Logger::Get().Debug("DisplayModule: 枚举到 ", result.size(), " 个活跃监视器");
    return result;
}

bool DisplayModule::GetCurrentPrimary(DisplayInfo &out) const
{
    auto displays = EnumerateDisplays();
    for (const auto &d : displays)
    {
        if (d.isPrimary)
        {
            out = d;
            return true;
        }
    }
    return false;
}

// ============================================================
//                      切换主显示器
// ============================================================

DisplayModule::SwitchResult DisplayModule::SwitchPrimaryDisplay(const std::string &displayId)
{
    auto displays = EnumerateDisplays();

    // 找到目标显示器
    const DisplayInfo *target = nullptr;
    const DisplayInfo *oldPrimary = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId)
            target = &d;
        if (d.isPrimary)
            oldPrimary = &d;
    }

    if (!target)
        return SwitchResult::NotFound;

    if (!target->isActive)
        return SwitchResult::NotActive;

    if (target->isPrimary)
        return SwitchResult::AlreadyPrimary;

    if (!oldPrimary)
    {
        Logger::Get().Info("DisplayModule: 未找到原主显示器，仍尝试切换");
    }

    Logger::Get().Info("DisplayModule: 切换主显示器 ", displayId,
                       " (原主: ", (oldPrimary ? oldPrimary->id : "none"), ")");

    // 策略：
    // 1. 把目标显示器设为 (0,0) + CDS_SET_PRIMARY
    // 2. 把原主显示器移到目标显示器右侧
    // 3. 其他显示器保持原位置（如果与 (0,0) 重叠，向右偏移到目标右侧）
    // 4. 应用所有更改

    std::wstring targetDevName(displayId.begin(), displayId.end());

    // 1. 设置目标显示器为主显示器
    DEVMODEW targetDm = {};
    targetDm.dmSize = sizeof(targetDm);
    targetDm.dmDriverExtra = 0;
    if (!EnumDisplaySettingsExW(targetDevName.c_str(), ENUM_CURRENT_SETTINGS, &targetDm, 0))
    {
        Logger::Get().Error("DisplayModule: 获取目标显示器当前模式失败");
        return SwitchResult::ApiFailed;
    }

    targetDm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    targetDm.dmPosition.x = 0;
    targetDm.dmPosition.y = 0;

    LONG r1 = ChangeDisplaySettingsExW(targetDevName.c_str(), &targetDm, nullptr,
                                       CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET,
                                       nullptr);
    if (r1 != DISP_CHANGE_SUCCESSFUL)
    {
        Logger::Get().Error("DisplayModule: 设置主显示器失败, code=", r1);
        return SwitchResult::ApiFailed;
    }

    // 2. 调整其他显示器的位置（避免与目标重叠）
    int nextX = targetDm.dmPelsWidth;  // 其他显示器排在目标右侧
    for (const auto &d : displays)
    {
        if (d.id == displayId)
            continue;

        std::wstring devName(d.id.begin(), d.id.end());
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
            continue;

        // 如果原位置与 (0,0) 重叠或为目标显示器区域，需要偏移
        int newX = d.x;
        int newY = d.y;
        if (newX < targetDm.dmPelsWidth && newX >= 0)
        {
            // 与目标显示器区域重叠，移到右侧
            newX = nextX;
            newY = 0;
            nextX += d.width;
        }

        dm.dmFields = DM_POSITION;
        dm.dmPosition.x = newX;
        dm.dmPosition.y = newY;
        ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr,
                                 CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    }

    // 3. 应用所有更改
    LONG r2 = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r2 != DISP_CHANGE_SUCCESSFUL)
    {
        Logger::Get().Error("DisplayModule: 应用更改失败, code=", r2);
        return SwitchResult::ApiFailed;
    }

    Logger::Get().Info("DisplayModule: 主显示器已切换为 ", displayId);

    // 强制刷新缓存，下次 MonitorLoop 会推送新状态
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_lastPrimaryId.clear();  // 强制触发变化检测
    }

    return SwitchResult::Ok;
}

// ============================================================
//                      枚举显示模式
// ============================================================

std::vector<DisplayModule::DisplayMode> DisplayModule::EnumerateModes(const std::string &displayId) const
{
    std::vector<DisplayMode> result;
    std::wstring devName(displayId.begin(), displayId.end());

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;

    DWORD i = 0;
    while (EnumDisplaySettingsExW(devName.c_str(), i, &dm, 0))
    {
        // 仅保留 32 位色深（低于 24 的过滤掉，避免列表过长）
        if (dm.dmBitsPerPel >= 24)
        {
            DisplayMode m;
            m.width = (int)dm.dmPelsWidth;
            m.height = (int)dm.dmPelsHeight;
            m.refreshRate = (int)dm.dmDisplayFrequency;
            m.bitsPerPel = (int)dm.dmBitsPerPel;

            // 去重（同 w/h/refresh/bpp）
            bool dup = false;
            for (const auto &e : result)
            {
                if (e.width == m.width && e.height == m.height &&
                    e.refreshRate == m.refreshRate && e.bitsPerPel == m.bitsPerPel)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                result.push_back(m);
        }
        ++i;

        // 限制枚举数量避免异常驱动导致死循环
        if (i > 1024)
            break;
    }

    // 排序：分辨率降序 → 刷新率降序
    std::sort(result.begin(), result.end(), [](const DisplayMode &a, const DisplayMode &b) {
        if (a.width != b.width) return a.width > b.width;
        if (a.height != b.height) return a.height > b.height;
        return a.refreshRate > b.refreshRate;
    });

    return result;
}

// ============================================================
//                      设置分辨率/刷新率
// ============================================================

DisplayModule::SetModeResult DisplayModule::SetDisplayMode(const std::string &displayId, int w, int h, int refresh)
{
    auto displays = EnumerateDisplays();
    const DisplayInfo *target = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId) { target = &d; break; }
    }
    if (!target) return SetModeResult::NotFound;
    if (!target->isActive) return SetModeResult::NotActive;

    // 验证模式是否在支持列表中
    auto modes = EnumerateModes(displayId);
    bool found = false;
    for (const auto &m : modes)
    {
        if (m.width == w && m.height == h && m.refreshRate == refresh)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        Logger::Get().Warning("DisplayModule: 模式不支持 ", w, "x", h, "@", refresh);
        return SetModeResult::ModeNotFound;
    }

    std::wstring devName(displayId.begin(), displayId.end());
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
    {
        Logger::Get().Error("DisplayModule: 获取当前模式失败");
        return SetModeResult::ApiFailed;
    }

    dm.dmPelsWidth = (DWORD)w;
    dm.dmPelsHeight = (DWORD)h;
    dm.dmDisplayFrequency = (DWORD)refresh;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    LONG r = ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr,
                                       CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL)
    {
        Logger::Get().Error("DisplayModule: 设置模式失败 code=", r);
        return SetModeResult::ApiFailed;
    }

    // 应用
    r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL)
    {
        Logger::Get().Error("DisplayModule: 应用模式失败 code=", r);
        return SetModeResult::ApiFailed;
    }

    Logger::Get().Info("DisplayModule: 已设置 ", displayId, " -> ", w, "x", h, "@", refresh);
    return SetModeResult::Ok;
}

// ============================================================
//                      设置缩放
// ============================================================

// scale 百分比 -> PerMonitorSettings 的 DpiValue 枚举值
// 0=100%, 1=125%, 2=150%, 3=175%, 4=200%, 5=225%, 6=250%, 7=300%
static int ScaleToDpiValue(int scale)
{
    switch (scale)
    {
    case 100: return 0;
    case 125: return 1;
    case 150: return 2;
    case 175: return 3;
    case 200: return 4;
    case 225: return 5;
    case 250: return 6;
    case 300: return 7;
    default:  return -1;
    }
}

// 从 "\\.\DISPLAY1" 提取显示器序号 1
static int ExtractMonitorIndex(const std::string &displayId)
{
    size_t i = displayId.size();
    while (i > 0 && displayId[i - 1] >= '0' && displayId[i - 1] <= '9')
        --i;
    if (i < displayId.size())
        return std::atoi(displayId.c_str() + i);
    return -1;
}

// 获取程序所在目录（含末尾 '\'）
static std::wstring GetExeDir()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t pos = path.find_last_of(L'\\');
    if (pos != std::wstring::npos)
        return path.substr(0, pos + 1);
    return L".\\";
}

// 调用 SetDPI.exe 设置缩放，返回 true 表示成功
// 用法: SetDPI.exe <scale> <monitor_index>
static bool CallSetDPI(int scale, int monitorIndex)
{
    std::wstring exeDir = GetExeDir();
    std::wstring setDpiPath = exeDir + L"SetDPI.exe";

    // 检测 SetDPI.exe 是否存在
    if (GetFileAttributesW(setDpiPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Logger::Get().Info("DisplayModule: SetDPI.exe 不存在于 ", WideToUtf8(exeDir),
                           "，回退到注册表方式");
        return false;
    }

    // 构造命令行: SetDPI.exe <scale> <monitorIndex>
    std::wstring cmd = L"\"" + setDpiPath + L"\" " + std::to_wstring(scale);
    if (monitorIndex > 0)
        cmd += L" " + std::to_wstring(monitorIndex);

    Logger::Get().Info("DisplayModule: 调用 ", WideToUtf8(cmd));

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, const_cast<LPWSTR>(cmd.c_str()),
                        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        Logger::Get().Error("DisplayModule: CreateProcess(SetDPI) 失败 code=", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 10000);  // 最多等 10 秒
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0)
    {
        Logger::Get().Error("DisplayModule: SetDPI.exe 退出码=", exitCode);
        return false;
    }

    Logger::Get().Info("DisplayModule: SetDPI.exe 设置成功 scale=", scale, " monitor=", monitorIndex);
    return true;
}

DisplayModule::SetScaleResult DisplayModule::SetDisplayScale(const std::string &displayId, int scale, bool *immediate)
{
    // 校验缩放值
    int dpiValue = ScaleToDpiValue(scale);
    if (dpiValue < 0) return SetScaleResult::InvalidScale;

    auto displays = EnumerateDisplays();
    const DisplayInfo *target = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId) { target = &d; break; }
    }
    if (!target) return SetScaleResult::NotFound;

    // 优先尝试 SetDPI.exe（即时生效，无需注销）
    int monitorIndex = ExtractMonitorIndex(displayId);
    if (CallSetDPI(scale, monitorIndex))
    {
        if (immediate) *immediate = true;
        return SetScaleResult::Ok;
    }

    // 回退到注册表方式（需要注销生效）
    if (immediate) *immediate = false;

    if (target->deviceId.empty())
    {
        Logger::Get().Error("DisplayModule: 显示器 deviceId 为空，无法设置 per-monitor 缩放");
        return SetScaleResult::RegistryFailed;
    }

    // DeviceID 中的 '\' 替换为 '#' 作为注册表键名
    std::string regKey = target->deviceId;
    for (char &c : regKey)
    {
        if (c == '\\') c = '#';
    }

    std::wstring regPath = L"Control Panel\\Desktop\\PerMonitorSettings\\";
    regPath += std::wstring(regKey.begin(), regKey.end());

    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(),
                             0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, &disposition);
    if (r != ERROR_SUCCESS)
    {
        Logger::Get().Error("DisplayModule: 打开/创建 PerMonitorSettings 注册表失败 code=", r,
                            " path=", regKey);
        return SetScaleResult::RegistryFailed;
    }

    DWORD val = (DWORD)dpiValue;
    r = RegSetValueExW(hKey, L"DpiValue", 0, REG_DWORD,
                       reinterpret_cast<const BYTE *>(&val), sizeof(val));
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS)
    {
        Logger::Get().Error("DisplayModule: 写入 DpiValue 失败 code=", r);
        return SetScaleResult::RegistryFailed;
    }

    Logger::Get().Info("DisplayModule: 已设置缩放(注册表) ", displayId, " -> ", scale,
                       "% (DpiValue=", dpiValue, ", key=", regKey, ", 需注销生效)");
    return SetScaleResult::Ok;
}

// ============================================================
//                      JSON 序列化
// ============================================================

std::string DisplayModule::DisplayToJson(const DisplayInfo &d) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\":\"" << EscapeJson(d.id) << "\",";
    ss << "\"device_id\":\"" << EscapeJson(d.deviceId) << "\",";
    ss << "\"name\":\"" << EscapeJson(d.name) << "\",";
    ss << "\"adapter\":\"" << EscapeJson(d.adapterName) << "\",";
    ss << "\"x\":" << d.x << ",";
    ss << "\"y\":" << d.y << ",";
    ss << "\"w\":" << d.width << ",";
    ss << "\"h\":" << d.height << ",";
    ss << "\"refresh\":" << d.refreshRate << ",";
    ss << "\"bpp\":" << d.bitsPerPel << ",";
    ss << "\"scale\":" << d.scale << ",";
    ss << "\"is_primary\":" << (d.isPrimary ? "true" : "false") << ",";
    ss << "\"is_active\":" << (d.isActive ? "true" : "false");
    ss << "}";
    return ss.str();
}

std::string DisplayModule::DisplaysToJson(const std::vector<DisplayInfo> &displays) const
{
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < displays.size(); ++i)
    {
        if (i > 0)
            ss << ",";
        ss << DisplayToJson(displays[i]);
    }
    ss << "]";
    return ss.str();
}

std::string DisplayModule::ModesToJson(const std::string &displayId,
                                        const std::vector<DisplayMode> &modes) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"display_id\":\"" << EscapeJson(displayId) << "\",";
    ss << "\"modes\":[";
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (i > 0) ss << ",";
        const auto &m = modes[i];
        ss << "{\"w\":" << m.width;
        ss << ",\"h\":" << m.height;
        ss << ",\"refresh\":" << m.refreshRate;
        ss << ",\"bpp\":" << m.bitsPerPel << "}";
    }
    ss << "]}";
    return ss.str();
}

// ============================================================
//                      指令处理
// ============================================================

void DisplayModule::OnCommand(SidebandSession &session,
                              uint32_t cmd_id,
                              uint32_t req_id,
                              const uint8_t *payload,
                              uint32_t payload_len)
{
    using namespace SidebandProtocol;

    switch (cmd_id)
    {
    case Cmd::DISPLAY_LIST_REQ:
    {
        auto displays = EnumerateDisplays();
        std::string json = DisplaysToJson(displays);
        std::vector<uint8_t> p(json.begin(), json.end());
        session.SendCommand(Cmd::DISPLAY_LIST_RESP, req_id, p);
        Logger::Get().Info("DisplayModule: 列出显示器 (count=", displays.size(), ")");
        break;
    }
    case Cmd::DISPLAY_SWITCH:
    {
        // payload 期望是 JSON: {"display_id":"\\\\.\\DISPLAY2"}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        Logger::Get().Info("DisplayModule: DISPLAY_SWITCH payload=", payloadStr);

        std::string displayId = ParseJsonStringField(payloadStr, "display_id");

        std::string respJson;
        if (displayId.empty())
        {
            respJson = R"({"ok":false,"error":"invalid_payload","msg":"missing display_id"})";
        }
        else
        {
            SwitchResult r = SwitchPrimaryDisplay(displayId);
            switch (r)
            {
            case SwitchResult::Ok:
                respJson = R"({"ok":true,"display_id":")" + EscapeJson(displayId) + R"("})";
                break;
            case SwitchResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found","display_id":")" + EscapeJson(displayId) + R"("})";
                break;
            case SwitchResult::AlreadyPrimary:
                respJson = R"({"ok":false,"error":"already_primary","display_id":")" + EscapeJson(displayId) + R"("})";
                break;
            case SwitchResult::NotActive:
                respJson = R"({"ok":false,"error":"not_active","display_id":")" + EscapeJson(displayId) + R"("})";
                break;
            case SwitchResult::ApiFailed:
                respJson = R"({"ok":false,"error":"api_failed","display_id":")" + EscapeJson(displayId) + R"("})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_CURRENT, req_id, p);

        // 切换成功后请求 MonitorLoop 异步广播新状态给所有客户端
        // （不直接调用 BroadcastCommand，因为 OnCommand 在 m_clientsMutex 持有期间被调用）
        if (respJson.find("\"ok\":true") != std::string::npos)
        {
            m_forcePush.store(true);
        }
        break;
    }
    case Cmd::DISPLAY_MODE_LIST_REQ:
    {
        // payload: JSON: {"display_id":"\\\\.\\DISPLAY1"}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = ParseJsonStringField(payloadStr, "display_id");

        std::string respJson;
        if (displayId.empty())
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            auto modes = EnumerateModes(displayId);
            respJson = "{\"ok\":true," + ModesToJson(displayId, modes).substr(1);
        }
        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_MODE_LIST_RESP, req_id, p);
        Logger::Get().Info("DisplayModule: 查询模式列表 ", displayId);
        break;
    }
    case Cmd::DISPLAY_MODE_SET:
    {
        // payload: JSON: {"display_id":"...","w":1920,"h":1080,"refresh":60}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = ParseJsonStringField(payloadStr, "display_id");
        int w = ParseJsonIntField(payloadStr, "w");
        int h = ParseJsonIntField(payloadStr, "h");
        int refresh = ParseJsonIntField(payloadStr, "refresh");

        std::string respJson;
        if (displayId.empty() || w <= 0 || h <= 0 || refresh <= 0)
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            SetModeResult r = SetDisplayMode(displayId, w, h, refresh);
            switch (r)
            {
            case SetModeResult::Ok:
                respJson = "{\"ok\":true,\"display_id\":\"" + EscapeJson(displayId) +
                           "\",\"w\":" + std::to_string(w) +
                           ",\"h\":" + std::to_string(h) +
                           ",\"refresh\":" + std::to_string(refresh) + "}";
                break;
            case SetModeResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found"})";
                break;
            case SetModeResult::NotActive:
                respJson = R"({"ok":false,"error":"not_active"})";
                break;
            case SetModeResult::ModeNotFound:
                respJson = R"({"ok":false,"error":"mode_not_found"})";
                break;
            case SetModeResult::ApiFailed:
                respJson = R"({"ok":false,"error":"api_failed"})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_CURRENT, req_id, p);

        // 分辨率变化后异步推送新状态
        if (respJson.find("\"ok\":true") != std::string::npos)
        {
            m_forcePush.store(true);
        }
        break;
    }
    case Cmd::DISPLAY_SCALE_SET:
    {
        // payload: JSON: {"display_id":"...","scale":125}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = ParseJsonStringField(payloadStr, "display_id");
        int scale = ParseJsonIntField(payloadStr, "scale");

        std::string respJson;
        if (displayId.empty() || scale <= 0)
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            bool immediate = false;
            SetScaleResult r = SetDisplayScale(displayId, scale, &immediate);
            switch (r)
            {
            case SetScaleResult::Ok:
                respJson = "{\"ok\":true,\"display_id\":\"" + EscapeJson(displayId) +
                           "\",\"scale\":" + std::to_string(scale) +
                           ",\"requires_sign_out\":" + (immediate ? "false" : "true") + "}";
                break;
            case SetScaleResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found"})";
                break;
            case SetScaleResult::InvalidScale:
                respJson = R"({"ok":false,"error":"invalid_scale","msg":"allowed: 100,125,150,175,200,225,250,300"})";
                break;
            case SetScaleResult::RegistryFailed:
                respJson = R"({"ok":false,"error":"registry_failed"})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        // 缩放不立即生效，用 DISPLAY_SCALE_SET 作为响应（客户端可识别）
        session.SendCommand(Cmd::DISPLAY_SCALE_SET, req_id, p);
        break;
    }
    default:
        Logger::Get().Debug("DisplayModule: 未处理的 cmd_id=", cmd_id);
        break;
    }
}

// ============================================================
//                      监控循环
// ============================================================

void DisplayModule::MonitorLoop()
{
    Logger::Get().Info("DisplayModule: 监控线程已启动");

    while (!m_exit)
    {
        try
        {
        // 处理客户端连接时的强制推送请求（不依赖 HasClients，确保首次推送）
        if (m_forcePush.exchange(false))
        {
            PushCurrentDisplayState(0);
            // 推送后更新缓存，避免下面又触发一次
            DisplayInfo primary;
            if (GetCurrentPrimary(primary))
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_lastHasPrimary = true;
                m_lastPrimaryId = primary.id;
                m_lastPrimaryWidth = primary.width;
                m_lastPrimaryHeight = primary.height;
                m_lastPrimaryRefresh = primary.refreshRate;
                m_lastPrimaryScale = primary.scale;
            }
        }

        if (m_server.HasClients())
        {
            DisplayInfo primary;
            bool hasPrimary = GetCurrentPrimary(primary);

            bool changed = false;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                if (hasPrimary != m_lastHasPrimary)
                {
                    changed = true;
                }
                else if (hasPrimary)
                {
                    if (primary.id != m_lastPrimaryId ||
                        primary.width != m_lastPrimaryWidth ||
                        primary.height != m_lastPrimaryHeight ||
                        primary.refreshRate != m_lastPrimaryRefresh ||
                        primary.scale != m_lastPrimaryScale)
                    {
                        changed = true;
                    }
                }

                if (changed)
                {
                    m_lastHasPrimary = hasPrimary;
                    if (hasPrimary)
                    {
                        m_lastPrimaryId = primary.id;
                        m_lastPrimaryWidth = primary.width;
                        m_lastPrimaryHeight = primary.height;
                        m_lastPrimaryRefresh = primary.refreshRate;
                        m_lastPrimaryScale = primary.scale;
                    }
                }
            }

            if (changed)
            {
                PushCurrentDisplayState(0);
            }
        }

        // 每 2 秒检查一次（频繁枚举显示器开销较大）
        for (int i = 0; i < 20 && !m_exit; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        catch (const std::exception &e)
        {
            Logger::Get().Error("DisplayModule: MonitorLoop 异常: ", e.what());
        }
        catch (...)
        {
            Logger::Get().Error("DisplayModule: MonitorLoop 未知异常");
        }
    }

    Logger::Get().Info("DisplayModule: 监控线程退出");
}

void DisplayModule::PushCurrentDisplayState(uint32_t req_id)
{
    DisplayInfo primary;
    if (!GetCurrentPrimary(primary))
    {
        std::string err = R"({"ok":false,"error":"no_primary"})";
        std::vector<uint8_t> p(err.begin(), err.end());
        m_server.BroadcastCommand(SidebandProtocol::Cmd::DISPLAY_CURRENT, req_id, p);
        return;
    }

    // 单条推送格式（与 DISPLAY_SWITCH 的响应一致）
    std::ostringstream ss;
    ss << "{";
    ss << "\"ok\":true,";
    ss << "\"display_id\":\"" << EscapeJson(primary.id) << "\",";
    ss << "\"name\":\"" << EscapeJson(primary.name) << "\",";
    ss << "\"w\":" << primary.width << ",";
    ss << "\"h\":" << primary.height << ",";
    ss << "\"refresh\":" << primary.refreshRate << ",";
    ss << "\"scale\":" << primary.scale;
    ss << "}";

    std::vector<uint8_t> p(ss.str().begin(), ss.str().end());
    m_server.BroadcastCommand(SidebandProtocol::Cmd::DISPLAY_CURRENT, req_id, p);

    Logger::Get().Debug("DisplayModule: 推送主显示器 ", primary.id,
                        " ", primary.width, "x", primary.height,
                        "@", primary.refreshRate, "Hz scale=", primary.scale, "%");
}
