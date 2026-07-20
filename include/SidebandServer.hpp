#pragma once

#include <winsock2.h>
#include <memory>
#include <vector>
#include <list>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <map>

#include "ISidebandModule.hpp"
#include "SidebandSession.hpp"

/**
 * 旁路服务主服务器
 *
 * 职责：
 * 1. 监听 TCP 5005 端口，接受 Android 客户端连接
 * 2. 管理客户端会话列表（线程安全）
 * 3. 提供广播 API 供模块调用
 * 4. 分发客户端指令到注册的模块
 * 5. 周期性 Tick 模块
 *
 * 兼容性：与原 windows-cursor-streamer 协议完全兼容，
 * 老客户端只能收光标包；新客户端可以额外发送控制指令。
 */
class SidebandServer
{
public:
    SidebandServer();
    ~SidebandServer();

    SidebandServer(const SidebandServer &) = delete;
    SidebandServer &operator=(const SidebandServer &) = delete;

    // 初始化 TCP 服务端
    bool Initialize(uint16_t port = SidebandProtocol::DEFAULT_PORT);

    // 关闭并清理
    void Shutdown();

    // 注册模块
    void RegisterModule(std::unique_ptr<ISidebandModule> module);

    // 启动主循环（阻塞）。主循环负责：
    // 1. 接受新连接
    // 2. 轮询所有客户端 socket 收取指令
    // 3. 周期性 Tick 所有模块
    void Run();

    // 请求退出主循环
    void RequestStop() { m_running = false; }

    // === 广播 API（模块调用） ===

    // 向所有客户端广播光标包（带缓存命中优化）
    void BroadcastCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                         int32_t frames, int32_t delay,
                         const std::vector<uint8_t> &pngData);

    // 向所有客户端广播文本光标状态
    void BroadcastTextCursorState(int32_t yPercentage);

    // 向所有客户端广播控制指令
    void BroadcastCommand(uint32_t cmd_id, uint32_t req_id,
                          const uint8_t *payload, uint32_t payload_len);
    void BroadcastCommand(uint32_t cmd_id, uint32_t req_id,
                          const std::vector<uint8_t> &payload)
    {
        BroadcastCommand(cmd_id, req_id, payload.data(), (uint32_t)payload.size());
    }

    // === 服务端 PNG 缓存（供 CursorModule 使用） ===
    bool GetCachedPng(uint32_t hash, std::vector<uint8_t> &outPng);
    void CachePng(uint32_t hash, const std::vector<uint8_t> &pngData);

    bool HasClients() const;

private:
    SOCKET m_listenSocket;
    std::atomic<bool> m_running;
    std::atomic<bool> m_initialized;

    // 客户端列表
    std::list<std::shared_ptr<SidebandSession>> m_clients;
    mutable std::mutex m_clientsMutex;

    // 模块列表
    std::vector<std::unique_ptr<ISidebandModule>> m_modules;

    // 服务端 PNG 缓存
    std::map<uint32_t, std::vector<uint8_t>> m_pngCache;
    std::mutex m_pngCacheMutex;

    // 客户端指令回调（分发给模块）
    void DispatchCommand(SidebandSession &session,
                         uint32_t cmd_id, uint32_t req_id,
                         const uint8_t *payload, uint32_t payload_len);

    // 客户端连接/断开通知
    void NotifyClientConnected(SidebandSession &session);
    void NotifyClientDisconnected(SidebandSession &session);

    // 周期性 tick
    void TickModules();

    // 清理已断开的客户端
    void CleanupDisconnectedClients();

    // 创建 SidebandSession 时的回调注入（设置 CommandCallback）
    void SetupSessionCallbacks(std::shared_ptr<SidebandSession> &session);
};
