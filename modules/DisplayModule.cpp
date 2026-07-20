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

    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);

    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
    {
        // 设备名形如 L"\\\\.\\DISPLAY1"
        std::string adapterId = WideToUtf8(adapter.DeviceName);
        std::string adapterName = WideToUtf8(adapter.DeviceString);

        // 枚举该适配器下的监视器
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);

        // 状态标志判断
        bool adapterActive = (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;

        // 获取当前显示模式
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        bool hasMode = EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS,
                                              &dm, 0) != FALSE;

        DisplayInfo info;
        info.id = adapterId;
        info.adapterName = adapterName;
        info.isActive = adapterActive && hasMode;

        // 尝试获取监视器名称（先取友好名称，失败再取接口名）
        bool gotMonitor = EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0) != FALSE;
        if (!gotMonitor)
        {
            monitor = {};
            monitor.cb = sizeof(monitor);
            gotMonitor = EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor,
                                              EDD_GET_DEVICE_INTERFACE_NAME) != FALSE;
        }
        if (gotMonitor)
        {
            info.name = WideToUtf8(monitor.DeviceString);
            if (info.name.empty())
                info.name = WideToUtf8(monitor.DeviceName);
        }
        if (info.name.empty())
            info.name = "Display " + std::to_string(i + 1);

        if (hasMode)
        {
            info.x = dm.dmPosition.x;
            info.y = dm.dmPosition.y;
            info.width = (int)dm.dmPelsWidth;
            info.height = (int)dm.dmPelsHeight;
            info.refreshRate = (int)dm.dmDisplayFrequency;
            info.bitsPerPel = (int)dm.dmBitsPerPel;
            // 主显示器 = 位于桌面原点 (0,0)
            info.isPrimary = (dm.dmPosition.x == 0 && dm.dmPosition.y == 0);
        }

        // 获取 DPI（缩放）
        if (info.isActive)
        {
            POINT pt = {dm.dmPosition.x + info.width / 2,
                        dm.dmPosition.y + info.height / 2};
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (hMon)
            {
                UINT dpiX = 96, dpiY = 96;
                if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
                {
                    info.scale = (int)((dpiX * 100 + 48) / 96);
                }
            }
        }

        if (info.isActive)
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
//                      JSON 序列化
// ============================================================

std::string DisplayModule::DisplayToJson(const DisplayInfo &d) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\":\"" << EscapeJson(d.id) << "\",";
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

        // 简易 JSON 解析：找 "display_id" 字段
        std::string displayId;
        size_t key = payloadStr.find("\"display_id\"");
        if (key != std::string::npos)
        {
            size_t colon = payloadStr.find(':', key);
            if (colon != std::string::npos)
            {
                size_t q1 = payloadStr.find('"', colon + 1);
                size_t q2 = payloadStr.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    displayId = payloadStr.substr(q1 + 1, q2 - q1 - 1);
            }
        }

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
