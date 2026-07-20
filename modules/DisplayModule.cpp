/**
 * DisplayModule 骨架实现
 *
 * 第一阶段仅返回 "not implemented" 错误响应，
 * 协议层完整可用，便于 Android 端先开发 UI 与协议解析。
 * 阶段 2 将填充实际功能（参见头文件注释）。
 */

#include "DisplayModule.hpp"
#include "Logger.hpp"
#include "SidebandProtocol.hpp"

#include <sstream>
#include <string>
#include <chrono>

DisplayModule::DisplayModule(SidebandServer &server) : m_server(server) {}

DisplayModule::~DisplayModule()
{
    Stop();
}

bool DisplayModule::Start()
{
    m_exit = false;
    // 阶段 2 启动监控线程
    // m_monitorThread = std::thread([this]() { MonitorLoop(); });
    Logger::Get().Info("DisplayModule: 已启动 (骨架模式, 阶段 2 待实现)");
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
    // 阶段 2：推送当前显示器状态
    Logger::Get().Debug("DisplayModule: 客户端连接 (骨架模式，无主动推送)");
}

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
        // 阶段 2：返回 QueryDisplayListAsJson()
        std::string err = R"({"error":"not_implemented","module":"Display","stage":2})";
        std::vector<uint8_t> errPayload(err.begin(), err.end());
        session.SendCommand(Cmd::DISPLAY_LIST_RESP, req_id, errPayload);
        Logger::Get().Info("DisplayModule: DISPLAY_LIST_REQ (骨架返回 not_implemented)");
        break;
    }
    case Cmd::DISPLAY_SWITCH:
    {
        // 阶段 2：解析 JSON 并调用 SwitchPrimaryDisplay
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        Logger::Get().Info("DisplayModule: DISPLAY_SWITCH payload=", payloadStr,
                           " (骨架模式，暂不执行)");
        // 返回失败响应（CmdID=DISPLAY_CURRENT 作为 ack）
        std::string err = R"({"error":"not_implemented","module":"Display","stage":2})";
        std::vector<uint8_t> errPayload(err.begin(), err.end());
        session.SendCommand(Cmd::DISPLAY_CURRENT, req_id, errPayload);
        break;
    }
    default:
        // 忽略未知指令
        Logger::Get().Debug("DisplayModule: 未处理的 cmd_id=", cmd_id);
        break;
    }
}

// === 阶段 2 待实现 ===

void DisplayModule::MonitorLoop()
{
    // TODO 阶段 2:
    // 1. 周期性调用 EnumDisplayDevices / GetMonitorInfo
    // 2. 检测主显示器变化
    // 3. 通过 m_server.BroadcastCommand(Cmd::DISPLAY_CURRENT, ...) 推送
    while (!m_exit)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

std::string DisplayModule::QueryDisplayListAsJson()
{
    // TODO 阶段 2: 枚举所有显示器
    // - EnumDisplayDevicesW(NULL, i, &dd, 0) 枚举适配器
    // - EnumDisplaySettingsEx 获取分辨率/刷新率
    // - GetDpiForMonitor 获取缩放
    return R"([{"id":"\\\\.\\DISPLAY1","name":"Primary","w":1920,"h":1080,"is_primary":true,"refresh":60,"scale":100}])";
}

bool DisplayModule::SwitchPrimaryDisplay(const std::string &displayId)
{
    // TODO 阶段 2:
    // 1. 枚举所有 display device 找到目标
    // 2. ChangeDisplaySettingsExW 切换主显示器
    //    DEVMODE dm = {}; dm.dmSize = sizeof(dm);
    //    dm.dmFields = DM_POSITION;
    //    ChangeDisplaySettingsExW(deviceName, &dm, NULL,
    //        CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    // 3. 通知所有其他 display 重新应用设置
    (void)displayId;
    return false;
}

std::string DisplayModule::QueryCurrentDisplayAsJson()
{
    // TODO 阶段 2: 返回当前主显示器信息
    return R"({"display_id":"\\\\.\\DISPLAY1","w":1920,"h":1080,"refresh":60,"scale":100})";
}
