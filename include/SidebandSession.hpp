#pragma once

#include <winsock2.h>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <mutex>
#include <atomic>

#include "SidebandProtocol.hpp"

/**
 * 客户端会话
 *
 * 封装一个 TCP 连接，提供：
 * 1. 发送光标包（兼容老协议）
 * 2. 发送控制指令包（新协议）
 * 3. 接收并解析客户端发来的控制指令
 *
 * 发送路径的关键约束（早期版本的 bug 来源）：
 * 非阻塞 socket 上 send() 可能返回一个**小于请求长度的正数**（部分发送）。
 * 早期实现只判断 SOCKET_ERROR，剩余字节被静默丢弃 —— 由于协议是长度前缀的，
 * 一旦发生半包，客户端后续解析就会永久错位。
 * 现在所有发送都经由 SendOrQueue()：要么整包送达，要么把剩余部分排进队列，
 * 绝不会只写出半个包。
 */
class SidebandSession
{
public:
    explicit SidebandSession(SOCKET sock);
    ~SidebandSession();

    SidebandSession(const SidebandSession &) = delete;
    SidebandSession &operator=(const SidebandSession &) = delete;

    SOCKET GetSocket() const { return m_socket; }
    bool IsConnected() const { return m_connected.load(std::memory_order_acquire); }

    // === 认证状态（分级授权）===
    bool IsAuthenticated() const { return m_authenticated.load(std::memory_order_acquire); }
    void SetAuthenticated(bool v) { m_authenticated.store(v, std::memory_order_release); }

    // 认证失败计数，用于简单的暴力破解限速
    int BumpAuthFailures() { return ++m_authFailures; }

    // === 发送 API ===

    // 发送光标包。是否走"缓存命中短包"由会话自己判断并记账 ——
    // 早期版本由 SidebandServer 在发送**之前**就把 hash 记入客户端缓存集合，
    // 一旦该帧因发送缓冲区满被丢弃，客户端就会在后续收到一个引用了它从未
    // 收到过的 PNG 的短包，从而渲染出错误的光标。现在只有确实交付
    //（直接发出或已排入队列）之后才记账。
    bool SendCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                    int32_t frames, int32_t delay,
                    const std::vector<uint8_t> &pngData);

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

    // socket 可写时调用，继续吐出排队的数据
    bool FlushSendQueue();

    // 是否有待发送的队列数据（用于决定是否向 WSAPoll 申请 POLLWRNORM）
    bool HasQueuedData() const;

    // === 接收 API ===

    // 尝试从 socket 读取数据并解析指令。
    // outDidWork: 本次是否真的读到了字节 —— 主循环据此判断是否算"有活干"。
    //             早期版本无条件把它当成有活干，导致只要有客户端连着，
    //             主循环就永远不休眠，空转吃满一个核。
    // 返回值：
    //   true  - 连接正常（无论是否解析出指令）
    //   false - 连接已断开或不可恢复
    bool TryReceive(bool &outDidWork);

    // 设置指令回调
    using CommandCallback = std::function<void(SidebandSession &, uint32_t, uint32_t, const uint8_t *, uint32_t)>;
    void SetCommandCallback(CommandCallback cb) { m_commandCallback = std::move(cb); }

    // 关闭连接
    void Close();

    // 对端地址（日志用）
    const std::string &PeerAddress() const { return m_peer; }
    void SetPeerAddress(std::string addr) { m_peer = std::move(addr); }

private:
    // 统一发送入口。
    // droppable=true（光标帧）：队列非空或发送缓冲区已满时整帧丢弃；
    //                          但若已写出一部分，剩余部分必须排队，否则流会错位。
    // droppable=false（控制指令）：保证送达与顺序，必要时全部排队。
    // 返回 false 表示连接已不可用；outDelivered 表示本包是否被接受。
    bool SendOrQueue(const std::vector<uint8_t> &packet, bool droppable, bool &outDelivered);

    // 不加锁的队列刷写，调用者需持有 m_sendMutex
    bool FlushSendQueueLocked();

    SOCKET m_socket;
    std::atomic<bool> m_connected;      // 主循环线程与广播线程都会访问，必须原子
    std::atomic<bool> m_authenticated;
    std::atomic<int> m_authFailures;
    std::string m_peer;

    // 光标协议缓存。
    // 只由 CursorModule 的光标工作线程经 BroadcastCursor -> SendCursor 访问，
    // 是单线程独占的，因此不额外加锁。
    //（注意：广播现在是在客户端列表锁**之外**做的，不能再依赖那把锁来保护它。
    //  若将来有第二个线程发送光标包，这里必须补锁。）
    std::unordered_set<uint32_t> m_cachedHashes;
    uint32_t m_lastSentHash;

    // 接收缓冲区（仅由主循环线程访问）
    std::vector<uint8_t> m_rxBuffer;
    CommandCallback m_commandCallback;

    // 发送队列
    std::vector<uint8_t> m_sendQueue;
    mutable std::mutex m_sendMutex;

    // 解析接收缓冲区中的完整包
    void ProcessRxBuffer();
    // 派发指令给回调
    void DispatchCommand(uint32_t cmd_id, uint32_t req_id,
                         const uint8_t *payload, uint32_t payload_len);
};
