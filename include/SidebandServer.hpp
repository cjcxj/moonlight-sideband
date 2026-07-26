#pragma once

#include <winsock2.h>
#include <memory>
#include <vector>
#include <list>
#include <string>
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
 * 1. 监听 TCP 端口，接受 Android 客户端连接
 * 2. 管理客户端会话列表（线程安全）
 * 3. 管理模块生命周期（Start/Stop）
 * 4. 提供广播 API 供模块调用
 * 5. 按 cmd_id 把指令**路由**到对应模块，并在此处统一做令牌鉴权
 * 6. 周期性 Tick 模块
 *
 * 兼容性：与原 windows-cursor-streamer 协议完全兼容 ——
 * 老客户端只收光标包、不发任何指令，因此不受鉴权影响。
 */
class SidebandServer
{
public:
    SidebandServer();
    ~SidebandServer();

    SidebandServer(const SidebandServer &) = delete;
    SidebandServer &operator=(const SidebandServer &) = delete;

    // 初始化 TCP 服务端。
    // loopbackOnly=true 时只绑定回环地址（局域网无法直连，需端口转发）。
    bool Initialize(uint16_t port = SidebandProtocol::DEFAULT_PORT,
                    bool loopbackOnly = false);

    // 关闭并清理
    void Shutdown();

    // 设置控制令牌（改变系统状态的指令需要客户端先提交它）
    void SetAuthToken(std::string token);

    // 注册模块（必须在 StartModules 之前调用）
    void RegisterModule(std::unique_ptr<ISidebandModule> module);

    // 启动/停止所有已注册模块
    void StartModules();
    void StopModules();

    // 启动主循环（阻塞）。主循环负责：
    // 1. 接受新连接
    // 2. 用 WSAPoll 等待客户端可读/可写事件（不再空转）
    // 3. 周期性 Tick 所有模块
    void Run();

    // 请求退出主循环
    void RequestStop() { m_running = false; }

    // 通知所有模块系统显示配置已变化（由主窗口 WM_DISPLAYCHANGE 调用）
    void NotifyDisplayChanged();

    // === 广播 API（模块调用） ===

    // 向所有客户端广播光标包（缓存命中判断与记账在会话内部完成）
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
    size_t GetClientCount() const;

private:
    using SessionPtr = std::shared_ptr<SidebandSession>;

    SOCKET m_listenSocket;
    std::atomic<bool> m_running;
    std::atomic<bool> m_initialized;

    // 客户端列表
    std::list<SessionPtr> m_clients;
    mutable std::mutex m_clientsMutex;

    // 模块列表
    std::vector<std::unique_ptr<ISidebandModule>> m_modules;
    bool m_modulesStarted = false;

    // 控制令牌
    std::string m_authToken;
    mutable std::mutex m_authMutex;

    // 服务端 PNG 缓存
    std::map<uint32_t, std::vector<uint8_t>> m_pngCache;
    std::mutex m_pngCacheMutex;

    // 接受所有待处理的新连接
    void AcceptNewClients();

    // 客户端指令入口：先处理通用指令，再鉴权，最后路由到唯一的处理模块
    void DispatchCommand(SidebandSession &session,
                         uint32_t cmd_id, uint32_t req_id,
                         const uint8_t *payload, uint32_t payload_len);

    // 处理 AUTH_REQ
    void HandleAuth(SidebandSession &session, uint32_t req_id,
                    const uint8_t *payload, uint32_t payload_len);

    // 客户端连接/断开通知（均在**未持锁**状态下调用）
    void NotifyClientConnected(SidebandSession &session);
    void NotifyClientDisconnected(SidebandSession &session);

    // 从列表中摘除已断开的会话并发出通知
    void ReapDisconnected();

    // 周期性 tick
    void TickModules();
};
