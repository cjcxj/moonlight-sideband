#pragma once

#include "ISidebandModule.hpp"
#include "SidebandSession.hpp"
#include "SidebandServer.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdint>

/**
 * AudioModule - 声音输出控制模块
 *
 * 对标 Windows 设置 → 系统 → 声音 → "选择播放声音的位置" 那一页：
 * 一份可切换的输出设备列表 + 每台设备的音量与静音开关。
 *
 * 已实现功能：
 * 1. 列出输出设备（CmdID=30/31）
 *    - WASAPI IMMDeviceEnumerator 枚举 eRender 端点
 *    - 名称取 PKEY_Device_FriendlyName（与设置面板显示的一致）
 *    - 每台设备带主音量、静音位、是否为默认设备
 *    - 返回 JSON: {"devices":[{id,name,volume,muted,is_default,is_default_comm,state},...]}
 *
 * 2. 切换默认输出设备（CmdID=32）
 *    - IPolicyConfig::SetDefaultEndpoint（未文档化 COM 接口，
 *      Windows 自己的设置面板走的也是它）
 *    - 同时设 eConsole 与 eMultimedia，**不动 eCommunications**：
 *      串流抓的是默认播放设备（eConsole/eMultimedia），而通讯角色留给
 *      麦克风/耳机，与设置面板的行为一致
 *    - 设完立刻回查 GetDefaultAudioEndpoint 比对端点 ID，防止假成功
 *
 * 3. 音量 / 静音（CmdID=33/34）
 *    - IAudioEndpointVolume 的 MasterVolumeLevelScalar，0-100 整数
 *      （与设置面板滑条同一套标度，不做对数换算）
 *
 * 4. 状态变化推送（CmdID=36）
 *    - 每 2 秒轮询一次并对整体做 hash 比较，仅在变化时广播
 *      （客户端连接、本机切换设备、拔插耳机都会触发）
 *
 * 写指令统一用 CmdID=35 回包：{"ok":true,...} 或 {"ok":false,"error":"..."}。
 */
class AudioModule : public ISidebandModule
{
public:
    struct DeviceInfo
    {
        std::string id;              // WASAPI 端点 ID，形如 {0.0.0.00000000}.{guid}
        std::string name;            // 友好名称（UTF-8）
        int volume = 0;              // 主音量 0-100
        bool muted = false;
        bool isDefault = false;      // eConsole 角色（即"默认设备"）
        bool isDefaultComm = false;  // eCommunications 角色
        std::string state = "active";
    };

    explicit AudioModule(SidebandServer &server);
    ~AudioModule() override;

    const char *GetName() const override { return "Audio"; }

    bool Start() override;
    void Stop() override;

    bool HandlesCommand(uint32_t cmd_id) const override;
    bool CommandRequiresAuth(uint32_t cmd_id) const override;
    uint32_t ResponseCommandFor(uint32_t cmd_id) const override;

    // 新客户端连上时推一次当前状态（与 DisplayModule 一致：不在主循环线程里枚举，
    // 只唤醒监控线程异步推送）
    void OnClientConnected(SidebandSession &) override;
    void OnClientDisconnected(SidebandSession &) override {}
    void OnCommand(SidebandSession &session,
                   uint32_t cmd_id,
                   uint32_t req_id,
                   const uint8_t *payload,
                   uint32_t payload_len) override;

private:
    enum class OpResult
    {
        Ok,
        NotFound,       // 端点 ID 不存在
        InvalidPayload, // 缺字段 / 音量越界
        ApiFailed,      // COM 调用失败
        NotApplied      // API 返回成功但回查发现默认设备没变
    };

    SidebandServer &m_server;
    std::atomic<bool> m_exit{false};
    std::atomic<bool> m_forcePush{false};

    // 监控线程：轮询设备状态，hash 变化才广播（与 DisplayModule 同一套路）
    std::thread m_monitorThread;
    std::mutex m_wakeMutex;
    std::condition_variable m_wakeCv;
    bool m_wakeRequested = false;

    // 写操作与轮询枚举可能同时访问 COM：串行化，避免一边切设备一边枚举出半成品列表
    mutable std::mutex m_audioMutex;

    std::vector<DeviceInfo> EnumerateDevices() const;
    OpResult SetDefaultDevice(const std::string &deviceId);
    OpResult SetVolume(const std::string &deviceId, int volume);
    OpResult SetMuted(const std::string &deviceId, bool muted);

    std::string DevicesToJson(const std::vector<DeviceInfo> &devices) const;
    uint64_t HashState(const std::vector<DeviceInfo> &devices) const;

    void MonitorLoop();
    void RequestPush();
    // 直接推 MonitorLoop 本轮枚举到的那份列表，不再二次枚举
    void PushState(const std::vector<DeviceInfo> &devices, uint32_t req_id);

    // 把 OpResult 翻成给客户端的 JSON（错误码是稳定契约，别随手改字面量）
    std::string OpResultToJson(OpResult r, const std::string &deviceId) const;
};
