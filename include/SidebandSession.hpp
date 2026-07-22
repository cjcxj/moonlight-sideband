#pragma once

#include <winsock2.h>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>

#include "SidebandProtocol.hpp"

/**
 * 客户端会话
 *
 * 封装一个 TCP 连接，提供：
 * 1. 发送光标包（兼容老协议）
 * 2. 发送控制指令包（新协议）
 * 3. 接收并解析客户端发来的控制指令
 */
class SidebandSession
{
public:
    explicit SidebandSession(SOCKET sock);
    ~SidebandSession();

    SidebandSession(const SidebandSession &) = delete;
    SidebandSession &operator=(const SidebandSession &) = delete;

    SOCKET GetSocket() const { return m_socket; }
    bool IsConnected() const { return m_connected; }

    // === 光标协议缓存（供 CursorModule 使用） ===
    std::unordered_set<uint32_t> &CachedHashes() { return m_cachedHashes; }
    uint32_t &LastSentHash() { return m_lastSentHash; }

    // === 发送 API ===

    // 发送光标包（兼容老协议）。isCacheHit=true 时只发短包。
    bool SendCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                    int32_t frames, int32_t delay,
                    const std::vector<uint8_t> &pngData, bool isCacheHit);

    // 发送文本光标状态（兼容老协议, CmdID=2）
    bool SendTextCursorState(int32_t yPercentage);

    // 发送控制指令（新协议）
    bool SendCommand(uint32_t cmd_id, uint32_t req_id,
                     const uint8_t *payload, uint32_t payload_len);
    bool SendCommand(uint32_t cmd_id, uint32_t req_id,
                     const std::vector<uint8_t> &payload)
    {
        return SendCommand(cmd_id, req_id, payload.data(), (uint32_t)payload.size());
    }

    // 刷新发送队列（WSAEWOULDBLOCK 时排队的数据）
    bool FlushSendQueue();

    // 是否有待发送的队列数据
    bool HasQueuedData() const;

    // === 接收 API ===

    // 尝试从 socket 读取数据并解析指令。
    // 返回值：
    //   true  - 连接正常（无论是否解析出指令）
    //   false - 连接已断开或不可恢复
    bool TryReceive();

    // 设置指令回调
    using CommandCallback = std::function<void(SidebandSession &, uint32_t, uint32_t, const uint8_t *, uint32_t)>;
    void SetCommandCallback(CommandCallback cb) { m_commandCallback = std::move(cb); }

    // 关闭连接
    void Close();

private:
    SOCKET m_socket;
    bool m_connected;

    // 光标协议缓存
    std::unordered_set<uint32_t> m_cachedHashes;
    uint32_t m_lastSentHash;

    // 接收缓冲区
    std::vector<uint8_t> m_rxBuffer;
    CommandCallback m_commandCallback;

    // 发送队列（WSAEWOULDBLOCK 时暂存控制指令）
    std::vector<uint8_t> m_sendQueue;
    mutable std::mutex m_sendMutex;

    // 解析接收缓冲区中的完整包
    void ProcessRxBuffer();
    // 派发指令给回调
    void DispatchCommand(uint32_t cmd_id, uint32_t req_id,
                         const uint8_t *payload, uint32_t payload_len);
};
