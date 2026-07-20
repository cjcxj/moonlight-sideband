#pragma once

#include "ISidebandModule.hpp"
#include "SidebandSession.hpp"
#include "SidebandServer.hpp"

#include <thread>
#include <atomic>
#include <mutex>

/**
 * DisplayModule - 显示器控制模块（骨架）
 *
 * 计划功能（阶段 2 实现）：
 * 1. 列出所有显示器（CmdID=10/11）
 *    - 通过 EnumDisplayDevices / EnumDisplayMonitors 枚举
 *    - 返回 JSON: [{id, name, w, h, is_primary, refresh_rate, scale}, ...]
 *
 * 2. 切换 Windows 主显示器（CmdID=12）
 *    - 通过 ChangeDisplaySettingsEx API
 *    - 立即生效，但 Sunshine 捕获的屏幕不会自动跟随
 *
 * 3. 当前显示器状态通知（CmdID=13）
 *    - 周期性监控主显示器变化
 *    - 返回 JSON: {display_id, w, h, refresh_rate, scale}
 *
 * 第一阶段仅注册指令处理器并回复 "not implemented"，
 * 保证协议层完整，方便 Android 端先开发 UI。
 */
class DisplayModule : public ISidebandModule
{
public:
    explicit DisplayModule(SidebandServer &server);
    ~DisplayModule() override;

    const char *GetName() const override { return "Display"; }

    void OnClientConnected(SidebandSession &session) override;
    void OnClientDisconnected(SidebandSession &session) override {}
    void OnCommand(SidebandSession &session,
                   uint32_t cmd_id,
                   uint32_t req_id,
                   const uint8_t *payload,
                   uint32_t payload_len) override;

    // 启动/停止（由 main 调用）
    bool Start();
    void Stop();

private:
    SidebandServer &m_server;
    std::atomic<bool> m_exit{false};

    // 显示器监控线程（用于检测主显示器变化并推送 CmdID=13）
    std::thread m_monitorThread;
    std::mutex m_mutex;

    // 上次已知的主显示器信息（用于变化检测）
    int m_lastPrimaryWidth = 0;
    int m_lastPrimaryHeight = 0;
    int m_lastPrimaryRefresh = 0;
    int m_lastPrimaryScale = 100;

    // 阶段 2 待实现
    void MonitorLoop();
    std::string QueryDisplayListAsJson();
    bool SwitchPrimaryDisplay(const std::string &displayId);
    std::string QueryCurrentDisplayAsJson();
};
