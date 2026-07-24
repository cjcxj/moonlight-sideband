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
#include <set>
#include <algorithm>
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
//                      CCD DPI 缩放（移植自 SetDPI）
// ============================================================

// DPI 缩放值表（与 Windows 设置一致）
static const UINT32 kDpiVals[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

// CCD 未公开的 DPI 查询/设置类型
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE =
    (DISPLAYCONFIG_DEVICE_INFO_TYPE)(-3);
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE DISPLAYCONFIG_DEVICE_INFO_SET_DPI_SCALE =
    (DISPLAYCONFIG_DEVICE_INFO_TYPE)(-4);

struct DISPLAYCONFIG_SOURCE_DPI_SCALE_GET
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t minScaleRel;   // 相对于推荐值的最小偏移
    int32_t curScaleRel;   // 相对于推荐值的当前偏移
    int32_t maxScaleRel;   // 相对于推荐值的最大偏移
};

struct DISPLAYCONFIG_SOURCE_DPI_SCALE_SET
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t scaleRel;      // 相对于推荐值的偏移
};

// 通过 GDI 设备名查找 CCD source 的 adapterId 和 sourceId
static bool FindSourceByGdiName(const std::wstring &gdiName,
                                LUID &outAdapterId, UINT32 &outSourceId)
{
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
        return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);

    if (QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    for (const auto &path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = path.sourceInfo.adapterId;
        srcName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(srcName.viewGdiDeviceName, gdiName.c_str()) == 0)
        {
            outAdapterId = path.sourceInfo.adapterId;
            outSourceId = path.sourceInfo.id;
            return true;
        }
    }
    return false;
}

// 获取当前 DPI 缩放百分比
static int GetDpiScalingPercent(LUID adapterId, UINT32 sourceId)
{
    DISPLAYCONFIG_SOURCE_DPI_SCALE_GET req = {};
    req.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE;
    req.header.size = sizeof(req);
    req.header.adapterId = adapterId;
    req.header.id = sourceId;

    if (DisplayConfigGetDeviceInfo(&req.header) != ERROR_SUCCESS)
        return 100;

    // 修正越界值
    if (req.curScaleRel < req.minScaleRel) req.curScaleRel = req.minScaleRel;
    if (req.curScaleRel > req.maxScaleRel) req.curScaleRel = req.maxScaleRel;

    int32_t minAbs = abs((int)req.minScaleRel);
    size_t idx = (size_t)(minAbs + req.curScaleRel);
    if (idx < sizeof(kDpiVals) / sizeof(kDpiVals[0]))
        return (int)kDpiVals[idx];

    return 100;
}

// 设置 DPI 缩放（即时生效）
static bool SetDpiScaling(LUID adapterId, UINT32 sourceId, int dpiPercent)
{
    // 获取当前 DPI 信息
    DISPLAYCONFIG_SOURCE_DPI_SCALE_GET req = {};
    req.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE;
    req.header.size = sizeof(req);
    req.header.adapterId = adapterId;
    req.header.id = sourceId;

    if (DisplayConfigGetDeviceInfo(&req.header) != ERROR_SUCCESS)
        return false;

    int32_t minAbs = abs((int)req.minScaleRel);

    // 查找目标百分比和推荐百分比在表中的索引
    int idxTarget = -1, idxRecommended = -1;
    for (int i = 0; i < (int)(sizeof(kDpiVals) / sizeof(kDpiVals[0])); ++i)
    {
        if ((int)kDpiVals[i] == dpiPercent) idxTarget = i;
        if (i == minAbs) idxRecommended = i;
    }

    if (idxTarget < 0 || idxRecommended < 0)
        return false;

    int32_t scaleRel = idxTarget - idxRecommended;

    DISPLAYCONFIG_SOURCE_DPI_SCALE_SET setReq = {};
    setReq.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_DPI_SCALE;
    setReq.header.size = sizeof(setReq);
    setReq.header.adapterId = adapterId;
    setReq.header.id = sourceId;
    setReq.scaleRel = scaleRel;

    return DisplayConfigSetDeviceInfo(&setReq.header) == ERROR_SUCCESS;
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

std::vector<DisplayModule::DisplayInfo> DisplayModule::EnumerateDisplays() const
{
    std::vector<DisplayInfo> result;

    // 用 CCD API 获取所有显示路径（只返回实际存在的物理路径，不含虚拟设备）
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
        return result;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);

    if (QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return result;

    // 活跃路径排前面，确保去重时优先保留活跃路径
    std::sort(paths.begin(), paths.end(), [](const DISPLAYCONFIG_PATH_INFO &a, const DISPLAYCONFIG_PATH_INFO &b) {
        bool aActive = (a.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
        bool bActive = (b.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
        return aActive && !bActive;
    });

    // 遍历 CCD 路径，提取显示器信息
    std::set<std::wstring> seenDevices;  // EDID 去重
    for (const auto &path : paths)
    {
        // 获取 source GDI 设备名（如 "\\.\DISPLAY1"）
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        std::wstring gdiName = sourceName.viewGdiDeviceName;
        if (gdiName.empty())
            continue;

        // 获取 target 友好名称和 EDID 信息
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        std::wstring friendlyName;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS)
        {
            friendlyName = targetName.monitorFriendlyDeviceName;
        }

        // 用 EDID 去重（同一物理显示器的不同接口路径 EDID 相同）
        std::wstring dedupKey;
        if (targetName.edidManufactureId != 0 || targetName.edidProductCodeId != 0)
        {
            dedupKey = std::to_wstring(targetName.edidManufactureId) + L"_" +
                       std::to_wstring(targetName.edidProductCodeId);
        }
        else if (targetName.monitorDevicePath[0] != L'\0')
        {
            dedupKey = targetName.monitorDevicePath;
        }
        else
        {
            dedupKey = gdiName;  // 回退到 GDI 设备名
        }

        if (seenDevices.count(dedupKey))
            continue;
        seenDevices.insert(dedupKey);

        DisplayInfo info;
        info.id = WideToUtf8(gdiName);
        info.name = WideToUtf8(friendlyName);
        // modeInfoIdx == INVALID 表示该 target 没有活跃模式（未启用）
        info.isActive = (path.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID);

        // 用 EnumDisplayDevicesW 获取适配器名称和监视器 DeviceID
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
        {
            if (_wcsicmp(adapter.DeviceName, gdiName.c_str()) == 0)
            {
                info.adapterName = WideToUtf8(adapter.DeviceString);
                break;
            }
        }

        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(gdiName.c_str(), 0, &monitor, 0))
        {
            info.deviceId = WideToUtf8(monitor.DeviceID);
            if (info.name.empty())
                info.name = WideToUtf8(monitor.DeviceString);
        }

        if (info.name.empty())
            info.name = "Display " + std::to_string(result.size() + 1);

        // 获取分辨率/刷新率
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
        {
            info.x = dm.dmPosition.x;
            info.y = dm.dmPosition.y;
            info.width = (int)dm.dmPelsWidth;
            info.height = (int)dm.dmPelsHeight;
            info.refreshRate = (int)dm.dmDisplayFrequency;
            info.bitsPerPel = (int)dm.dmBitsPerPel;
            info.isPrimary = (dm.dmPosition.x == 0 && dm.dmPosition.y == 0);
        }
        else if (EnumDisplaySettingsExW(gdiName.c_str(), ENUM_REGISTRY_SETTINGS, &dm, 0))
        {
            info.x = dm.dmPosition.x;
            info.y = dm.dmPosition.y;
            info.width = (int)dm.dmPelsWidth;
            info.height = (int)dm.dmPelsHeight;
            info.refreshRate = (int)dm.dmDisplayFrequency;
            info.bitsPerPel = (int)dm.dmBitsPerPel;
        }

        // 用 CCD API 获取 DPI 缩放（对活跃和未启用的显示器都有效）
        info.scale = GetDpiScalingPercent(path.sourceInfo.adapterId, path.sourceInfo.id);

        Logger::Get().Debug("DisplayModule: ", info.id, " active=", info.isActive,
                            " ", info.width, "x", info.height, "@", info.refreshRate,
                            " name=", info.name);

        result.push_back(info);
    }

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
//                      切换主显示器（单显示器模式）
// ============================================================

DisplayModule::SwitchResult DisplayModule::SwitchPrimaryDisplay(const std::string &displayId)
{
    auto displays = EnumerateDisplays();

    // 找到目标显示器
    const DisplayInfo *target = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId) { target = &d; break; }
    }

    if (!target)
        return SwitchResult::NotFound;

    if (target->isPrimary && target->isActive)
        return SwitchResult::AlreadyPrimary;

    Logger::Get().Info("DisplayModule: 切换到单显示器模式 ", displayId);

    std::wstring targetDevName(displayId.begin(), displayId.end());

    // 策略：仅启用目标显示器，禁用其他所有显示器
    // 1. 启用目标显示器，设为 (0,0) + CDS_SET_PRIMARY
    // 2. 禁用其他显示器（dmPelsWidth=0, dmPelsHeight=0）
    // 3. 应用所有更改

    // 1. 启用目标显示器
    DEVMODEW targetDm = {};
    targetDm.dmSize = sizeof(targetDm);
    targetDm.dmDriverExtra = 0;

    // 优先用当前模式，失败则用注册表模式（适用于从未启用状态切换）
    if (!EnumDisplaySettingsExW(targetDevName.c_str(), ENUM_CURRENT_SETTINGS, &targetDm, 0))
    {
        if (!EnumDisplaySettingsExW(targetDevName.c_str(), ENUM_REGISTRY_SETTINGS, &targetDm, 0))
        {
            Logger::Get().Error("DisplayModule: 获取目标显示器模式失败");
            return SwitchResult::ApiFailed;
        }
    }

    targetDm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
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

    // 2. 禁用其他所有显示器
    for (const auto &d : displays)
    {
        if (d.id == displayId)
            continue;

        std::wstring devName(d.id.begin(), d.id.end());
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;

        // 获取当前或注册表模式作为基础（保留分辨率信息，只改位置和状态）
        if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
            EnumDisplaySettingsExW(devName.c_str(), ENUM_REGISTRY_SETTINGS, &dm, 0);

        // 设置为禁用状态：PelsWidth=0, PelsHeight=0
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        dm.dmPelsWidth = 0;
        dm.dmPelsHeight = 0;
        dm.dmPosition.x = 0;
        dm.dmPosition.y = 0;

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

    Logger::Get().Info("DisplayModule: 已切换到 ", displayId, " (其他显示器已禁用)");

    // 强制刷新缓存，下次 MonitorLoop 会推送新状态
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_lastPrimaryId.clear();
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
//                      设置缩放（CCD API 即时生效）
// ============================================================

// 校验缩放值是否在支持列表中
static bool IsValidDpiScale(int scale)
{
    for (auto v : kDpiVals)
    {
        if ((int)v == scale) return true;
    }
    return false;
}

DisplayModule::SetScaleResult DisplayModule::SetDisplayScale(const std::string &displayId, int scale, bool *immediate)
{
    if (!IsValidDpiScale(scale))
        return SetScaleResult::InvalidScale;

    // 通过 GDI 设备名查找 CCD source
    std::wstring devName(displayId.begin(), displayId.end());
    LUID adapterId = {};
    UINT32 sourceId = 0;
    if (!FindSourceByGdiName(devName, adapterId, sourceId))
    {
        Logger::Get().Error("DisplayModule: 找不到显示器 ", displayId, " 的 CCD source");
        return SetScaleResult::NotFound;
    }

    if (SetDpiScaling(adapterId, sourceId, scale))
    {
        if (immediate) *immediate = true;
        Logger::Get().Info("DisplayModule: 已设置缩放 ", displayId, " -> ", scale,
                           "% (CCD API, 即时生效)");
        return SetScaleResult::Ok;
    }

    Logger::Get().Error("DisplayModule: CCD API 设置缩放失败 ", displayId, " -> ", scale, "%");
    if (immediate) *immediate = false;
    return SetScaleResult::RegistryFailed;
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
                respJson = R"({"ok":false,"error":"invalid_scale","msg":"allowed: 100,125,150,175,200,225,250,300,350,400,450,500"})";
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
