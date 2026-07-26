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
#include <condition_variable>

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
 *
 * 4. 查询显示器支持的模式列表（CmdID=14/15）
 *    - 通过 EnumDisplaySettingsExW + ENUM_DISPLAY_SETTINGS_MODES 枚举
 *    - 返回 JSON: {display_id, modes:[{w,h,refresh,bpp},...]}
 *    - 模式去重（同分辨率+刷新率只保留一个）
 *
 * 5. 设置分辨率/刷新率（CmdID=16）
 *    - 通过 ChangeDisplaySettingsExW + CDS_UPDATEREGISTRY
 *    - 立即生效
 *    - 返回 JSON: {ok, display_id, w, h, refresh} 或 {ok:false, error}
 *
 * 6. 设置缩放（CmdID=17）
 *    - 使用 CCD API DisplayConfigSetDeviceInfo 即时生效（无需注销）
 *    - 移植自 SetDPI 工具（github.com/imniko/SetDPI）
 *    - 支持的缩放值：100,125,150,175,200,225,250,300,350,400,450,500
 *    - 返回 JSON: {ok, display_id, scale, requires_sign_out:false} 或 {ok:false, error}
 */
class DisplayModule : public ISidebandModule
{
public:
    // 显示器信息结构
    struct DisplayInfo
    {
        std::string id;          // "\\\\.\\DISPLAY1"（GDI 设备名，用于 ChangeDisplaySettingsExW）
        std::string deviceId;    // 监视器设备 ID（如 "MONITOR\\SAM0F91\\5&..."，用于 PerMonitorSettings 注册表路径）
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

    // 显示模式（分辨率+刷新率+色深）
    struct DisplayMode
    {
        int width = 0;
        int height = 0;
        int refreshRate = 0;
        int bitsPerPel = 0;
    };

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

    // 生命周期（现在是 ISidebandModule 接口的一部分）
    bool Start() override;
    void Stop() override;

    // 本模块负责的指令
    bool HandlesCommand(uint32_t cmd_id) const override;

    // 会改变系统状态的指令要求客户端已认证：
    // 切换显示器 / 设置分辨率 / 设置缩放。只读查询不需要。
    bool CommandRequiresAuth(uint32_t cmd_id) const override;

    // 收到 WM_DISPLAYCHANGE（UI 线程）——只唤醒工作线程，不在此枚举
    void OnDisplayChanged() override;

private:

    SidebandServer &m_server;
    std::atomic<bool> m_exit{false};
    std::atomic<bool> m_forcePush{false};  // 客户端连接/切换完成时置位，由 MonitorLoop 异步推送

    // 显示器监控线程。
    // 早期实现是每 2 秒无条件枚举一次所有显示器（单次约 50ms）来发现变化；
    // 现在改为等待条件变量：由 WM_DISPLAYCHANGE 或 m_forcePush 唤醒，
    // 另有一个 10 秒的兜底超时以防漏掉某些不发广播的变更（如仅缩放改变）。
    std::thread m_monitorThread;
    std::mutex m_wakeMutex;
    std::condition_variable m_wakeCv;
    bool m_wakeRequested = false;
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

    // 枚举某显示器支持的所有模式（去重）
    std::vector<DisplayMode> EnumerateModes(const std::string &displayId) const;

    // 设置显示器分辨率/刷新率
    enum class SetModeResult
    {
        Ok,
        NotFound,
        NotActive,
        ModeNotFound,
        ApiFailed
    };
    SetModeResult SetDisplayMode(const std::string &displayId, int w, int h, int refresh);

    // 设置缩放（写注册表）
    enum class SetScaleResult
    {
        Ok,
        NotFound,
        InvalidScale,
        RegistryFailed
    };
    SetScaleResult SetDisplayScale(const std::string &displayId, int scale, bool *immediate = nullptr);

    // 获取当前主显示器信息（不含完整列表）
    bool GetCurrentPrimary(DisplayInfo &out) const;

    // 轮询验证指定 target 已激活（切换是异步生效的，用于避免假成功）
    bool VerifyTargetActive(uint32_t targetId, int attempts, int intervalMs) const;

    // 把 DisplayInfo 列表序列化为 JSON
    std::string DisplaysToJson(const std::vector<DisplayInfo> &displays) const;

    // 把单个 DisplayInfo 序列化为 JSON
    std::string DisplayToJson(const DisplayInfo &d) const;

    // 把 DisplayMode 列表序列化为 JSON
    std::string ModesToJson(const std::string &displayId, const std::vector<DisplayMode> &modes) const;

    // 监控主显示器变化的循环
    void MonitorLoop();

    // 唤醒监控线程（置 m_forcePush 并 notify）
    void RequestPush();

    // 推送当前主显示器状态（通过 BroadcastCommand）
    void PushCurrentDisplayState(uint32_t req_id = 0);
};

// 说明：早期版本在这里导出了一组**全局命名空间**的自由函数
//（WideToUtf8 / EscapeJson / ParseJsonStringField / ParseJsonIntField），
// 既有撞名风险，JSON 解析本身也很脆（会匹配到出现在值内部的键名）。
// 现已拆走：字符串转换见 include/Win32Util.hpp，JSON 见 include/Json.hpp。
