#pragma once

#include "ISidebandModule.hpp"
#include "SidebandSession.hpp"
#include "SidebandServer.hpp"

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

/**
 * DisplayModule - 显示器控制模块
 *
 * 已实现功能：
 * 1. 列出所有显示器（CmdID=10/11）
 *    - 通过 EnumDisplayDevicesW 枚举适配器与监视器
 *    - 通过 EnumDisplaySettingsExW 获取分辨率/刷新率/色深
 *    - 通过 GetDpiForMonitor 获取缩放
 *    - 返回 JSON: [{id,name,adapter,x,y,w,h,refresh,bpp,scale,is_primary,is_active}, ...]
 *
 * 2. 切换 Windows 主显示器（CmdID=12）
 *    - 通过 ChangeDisplaySettingsExW + CDS_SET_PRIMARY
 *    - 自动调整其他显示器的相对位置避免重叠
 *    - 立即生效（不需要重启 Sunshine）
 *    - 注意：Sunshine 捕获的屏幕不会自动跟随，这是 Sunshine 限制
 *
 * 3. 当前显示器状态通知（CmdID=13）
 *    - 周期性监控主显示器变化（分辨率/刷新率/缩放）
 *    - 变化时通过 BroadcastCommand 推送
 *    - 客户端连接时立即推送一次当前状态
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
    // 显示器信息结构
    struct DisplayInfo
    {
        std::string id;          // "\\\\.\\DISPLAY1"
        std::string name;        // 监视器名称（如 "DELL U2720Q"）
        std::string adapterName; // 适配器名称（如 "NVIDIA GeForce RTX 3060"）
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int refreshRate = 0;     // Hz
        int bitsPerPel = 0;      // 色深
        int scale = 100;         // 缩放百分比 (100/125/150/200)
        bool isPrimary = false;
        bool isActive = false;
    };

    SidebandServer &m_server;
    std::atomic<bool> m_exit{false};
    std::atomic<bool> m_forcePush{false};  // 客户端连接时置位，由 MonitorLoop 异步推送

    // 显示器监控线程
    std::thread m_monitorThread;
    mutable std::mutex m_mutex;

    // 上次已知的主显示器信息（用于变化检测）
    std::string m_lastPrimaryId;
    int m_lastPrimaryWidth = 0;
    int m_lastPrimaryHeight = 0;
    int m_lastPrimaryRefresh = 0;
    int m_lastPrimaryScale = 100;
    bool m_lastHasPrimary = false;

    // === 内部实现 ===

    // 枚举所有显示器
    std::vector<DisplayInfo> EnumerateDisplays() const;

    // 切换主显示器
    enum class SwitchResult
    {
        Ok,
        NotFound,
        AlreadyPrimary,
        NotActive,
        ApiFailed
    };
    SwitchResult SwitchPrimaryDisplay(const std::string &displayId);

    // 获取当前主显示器信息（不含完整列表）
    bool GetCurrentPrimary(DisplayInfo &out) const;

    // 把 DisplayInfo 列表序列化为 JSON
    std::string DisplaysToJson(const std::vector<DisplayInfo> &displays) const;

    // 把单个 DisplayInfo 序列化为 JSON
    std::string DisplayToJson(const DisplayInfo &d) const;

    // 监控主显示器变化的循环
    void MonitorLoop();

    // 推送当前主显示器状态（通过 BroadcastCommand）
    void PushCurrentDisplayState(uint32_t req_id = 0);
};

// === 全局辅助函数 ===

// 宽字符转 UTF-8
std::string WideToUtf8(const std::wstring &w);

// JSON 字符串转义
std::string EscapeJson(const std::string &s);
