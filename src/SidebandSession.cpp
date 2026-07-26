#include "SidebandSession.hpp"
#include "Logger.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <cstring>
#include <algorithm>

SidebandSession::SidebandSession(SOCKET sock)
    : m_socket(sock), m_connected(true), m_authenticated(false),
      m_authFailures(0), m_lastSentHash(0)
{
    // 禁用 Nagle 算法，低延迟
    int yes = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&yes, sizeof(yes));

    // 开启 TCP keepalive 并调短探测间隔。
    // 手机休眠/掉 Wi-Fi 这类"半开连接"如果不探测，会一直挂在客户端列表里
    // 直到系统默认的 2 小时超时，期间还在往里写光标数据。
    setsockopt(m_socket, SOL_SOCKET, SO_KEEPALIVE, (char *)&yes, sizeof(yes));

    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = 30000;      // 空闲 30s 后开始探测
    ka.keepaliveinterval = 5000;   // 每 5s 探测一次
    DWORD bytesReturned = 0;
    WSAIoctl(m_socket, SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
             nullptr, 0, &bytesReturned, nullptr, nullptr);
}

SidebandSession::~SidebandSession()
{
    Close();
}

// ============================================================
//                      发送
// ============================================================

bool SidebandSession::SendOrQueue(const std::vector<uint8_t> &packet,
                                  bool droppable, bool &outDelivered)
{
    outDelivered = false;

    if (!IsConnected())
        return false;

    std::lock_guard<std::mutex> lk(m_sendMutex);

    // 队列里还有东西 —— 必须先排空，否则新包会插到旧包前面把流搞乱
    if (!m_sendQueue.empty())
    {
        if (!FlushSendQueueLocked())
            return false;

        if (!m_sendQueue.empty())
        {
            // 仍未排空：可丢弃的帧（光标）直接丢，控制指令则继续排队
            if (droppable)
                return true;

            if (m_sendQueue.size() + packet.size() > SidebandProtocol::MAX_SEND_QUEUE_BYTES)
            {
                Logger::Get().Warning("SidebandSession: 发送队列超限(",
                                    m_sendQueue.size(), " 字节)，判定客户端失速并断开 ", m_peer);
                m_connected.store(false, std::memory_order_release);
                m_sendQueue.clear();
                return false;
            }

            m_sendQueue.insert(m_sendQueue.end(), packet.begin(), packet.end());
            outDelivered = true;
            return true;
        }
    }

    // 队列已空，尝试直接发送
    size_t offset = 0;
    while (offset < packet.size())
    {
        int sent = send(m_socket, (const char *)packet.data() + offset,
                        (int)(packet.size() - offset), 0);

        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                Logger::Get().Debug("SidebandSession: send 失败 WSA=", err, " ", m_peer);
                m_connected.store(false, std::memory_order_release);
                return false;
            }

            // 发送缓冲区满
            if (offset == 0 && droppable)
                return true;   // 一个字节都还没写出去，整帧丢掉是安全的

            // 已经写出了一部分 —— 剩余部分必须排队，否则接收端的
            // 长度前缀解析会永久错位。控制指令同理必须排队。
            if (m_sendQueue.size() + (packet.size() - offset) >
                SidebandProtocol::MAX_SEND_QUEUE_BYTES)
            {
                Logger::Get().Warning("SidebandSession: 发送队列超限，断开 ", m_peer);
                m_connected.store(false, std::memory_order_release);
                m_sendQueue.clear();
                return false;
            }

            m_sendQueue.insert(m_sendQueue.end(),
                               packet.begin() + offset, packet.end());
            outDelivered = true;
            return true;
        }

        if (sent <= 0)
        {
            m_connected.store(false, std::memory_order_release);
            return false;
        }

        offset += (size_t)sent;   // 部分发送：继续把剩下的写完
    }

    outDelivered = true;
    return true;
}

bool SidebandSession::SendCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                                 int32_t frames, int32_t delay,
                                 const std::vector<uint8_t> &pngData)
{
    if (!IsConnected())
        return false;

    // 与上一帧同一个光标，无需重复发送
    if (m_lastSentHash == hash)
        return true;

    const bool isCacheHit = (m_cachedHashes.find(hash) != m_cachedHashes.end());

    std::vector<uint8_t> packet = isCacheHit
        ? SidebandProtocol::BuildCachedCursorPacket(hash, hotX, hotY, frames, delay)
        : SidebandProtocol::BuildCursorPacket(hash, hotX, hotY, frames, delay, pngData);

    bool delivered = false;
    if (!SendOrQueue(packet, /*droppable=*/true, delivered))
        return false;

    // 只有确实交付了才记账。否则下次同一个 hash 会被误判成"客户端已缓存"，
    // 从而只发短包，而客户端根本没收到过对应的 PNG。
    if (delivered)
    {
        if (!isCacheHit)
        {
            m_cachedHashes.insert(hash);
            if (m_cachedHashes.size() > 100)
            {
                // 缓存集合清空后，客户端侧缓存也应视为失效：
                // 清掉 lastSentHash 以便下一帧重新走全量包
                m_cachedHashes.clear();
                m_lastSentHash = 0;
                return true;
            }
        }
        m_lastSentHash = hash;
    }
    return true;
}

bool SidebandSession::SendTextCursorState(int32_t yPercentage)
{
    if (!IsConnected())
        return false;

    auto packet = SidebandProtocol::BuildTextCursorPacket(yPercentage);
    bool delivered = false;
    return SendOrQueue(packet, /*droppable=*/true, delivered);
}

bool SidebandSession::SendCommand(uint32_t cmd_id, uint32_t req_id,
                                  const uint8_t *payload, uint32_t payload_len)
{
    if (!IsConnected())
        return false;

    auto packet = SidebandProtocol::BuildCommandPacket(cmd_id, req_id, payload, payload_len);
    bool delivered = false;
    // 控制指令不可丢弃
    return SendOrQueue(packet, /*droppable=*/false, delivered);
}

bool SidebandSession::FlushSendQueueLocked()
{
    while (!m_sendQueue.empty())
    {
        int sent = send(m_socket, (const char *)m_sendQueue.data(),
                        (int)m_sendQueue.size(), 0);
        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return true;  // 缓冲区仍满，下次可写时再试

            Logger::Get().Debug("SidebandSession: flush 失败 WSA=", err, " ", m_peer);
            m_connected.store(false, std::memory_order_release);
            m_sendQueue.clear();
            return false;
        }
        if (sent <= 0)
        {
            m_connected.store(false, std::memory_order_release);
            m_sendQueue.clear();
            return false;
        }
        m_sendQueue.erase(m_sendQueue.begin(), m_sendQueue.begin() + sent);
    }
    return true;
}

bool SidebandSession::FlushSendQueue()
{
    if (!IsConnected())
        return false;
    std::lock_guard<std::mutex> lk(m_sendMutex);
    return FlushSendQueueLocked();
}

bool SidebandSession::HasQueuedData() const
{
    std::lock_guard<std::mutex> lk(m_sendMutex);
    return !m_sendQueue.empty();
}

// ============================================================
//                      接收
// ============================================================

bool SidebandSession::TryReceive(bool &outDidWork)
{
    outDidWork = false;

    if (!IsConnected())
        return false;

    // 一次把内核缓冲区读干净，而不是每轮循环只读 4KB
    for (;;)
    {
        char buf[8192];
        int received = recv(m_socket, buf, sizeof(buf), 0);

        if (received == 0)
        {
            // 客户端正常关闭
            m_connected.store(false, std::memory_order_release);
            return false;
        }

        if (received == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                break;  // 读干净了

            Logger::Get().Debug("SidebandSession: recv 失败 WSA=", err, " ", m_peer);
            m_connected.store(false, std::memory_order_release);
            return false;
        }

        outDidWork = true;
        m_rxBuffer.insert(m_rxBuffer.end(), buf, buf + received);

        // 接收缓冲区总量兜底（正常情况下 ProcessRxBuffer 会及时消费）
        if (m_rxBuffer.size() > SidebandProtocol::MAX_BODY_LEN + 4096)
        {
            Logger::Get().Warning("SidebandSession: 接收缓冲区异常膨胀，断开 ", m_peer);
            m_connected.store(false, std::memory_order_release);
            m_rxBuffer.clear();
            return false;
        }

        if ((size_t)received < sizeof(buf))
            break;  // 内核里没有更多数据了
    }

    if (outDidWork)
        ProcessRxBuffer();

    return IsConnected();
}

void SidebandSession::ProcessRxBuffer()
{
    // 协议格式:
    //   [BodyLen(4)] [Body(BodyLen)]
    // Body 内部:
    //   [Hash(4)] [CmdID(4)] [ReqID(4)] [PayloadLen(4)] [Payload...]
    //   (Hash == 0xFFFFFFFF)

    size_t consumed = 0;

    while (m_rxBuffer.size() - consumed >= 4)
    {
        const uint8_t *base = m_rxBuffer.data() + consumed;
        const size_t avail = m_rxBuffer.size() - consumed;

        uint32_t bodyLen;
        memcpy(&bodyLen, base, 4);

        // 合理性检查：避免恶意大包导致内存爆炸
        if (bodyLen > SidebandProtocol::MAX_BODY_LEN)
        {
            Logger::Get().Warning("SidebandSession: 收到超大包 BodyLen=", bodyLen,
                                "，断开连接 ", m_peer);
            m_connected.store(false, std::memory_order_release);
            m_rxBuffer.clear();
            return;
        }

        // 数据还没收齐
        if (avail < (size_t)4 + bodyLen)
            break;

        const uint8_t *body = base + 4;
        const size_t packetSize = (size_t)4 + bodyLen;

        // 客户端发到 PC 的只允许是控制指令包（Hash == MAGIC_HASH）
        if (bodyLen < 4)
        {
            Logger::Get().Debug("SidebandSession: BodyLen < 4，跳过");
            consumed += packetSize;
            continue;
        }

        uint32_t hash;
        memcpy(&hash, body, 4);

        if (hash != SidebandProtocol::MAGIC_HASH)
        {
            Logger::Get().Debug("SidebandSession: 收到非控制包 hash=", hash, "，忽略");
            consumed += packetSize;
            continue;
        }

        if (bodyLen < SidebandProtocol::COMMAND_HEADER_SIZE)
        {
            Logger::Get().Debug("SidebandSession: 控制包头不足, BodyLen=", bodyLen);
            consumed += packetSize;
            continue;
        }

        uint32_t cmd_id, req_id, payload_len;
        memcpy(&cmd_id, body + 4, 4);
        memcpy(&req_id, body + 8, 4);
        memcpy(&payload_len, body + 12, 4);

        // payload 长度校验
        if (bodyLen != SidebandProtocol::COMMAND_HEADER_SIZE + payload_len)
        {
            Logger::Get().Debug("SidebandSession: PayloadLen 不匹配, declared=", payload_len,
                                " actual=", bodyLen - SidebandProtocol::COMMAND_HEADER_SIZE);
            consumed += packetSize;
            continue;
        }

        const uint8_t *payload = (payload_len > 0) ? (body + 16) : nullptr;
        DispatchCommand(cmd_id, req_id, payload, payload_len);

        consumed += packetSize;

        // 指令处理过程中可能已判定断开（如认证失败次数超限）
        if (!IsConnected())
        {
            m_rxBuffer.clear();
            return;
        }
    }

    // 一次性丢弃已消费的前缀，避免每个包都做一次 O(n) 的 memmove
    if (consumed > 0)
        m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + consumed);
}

void SidebandSession::DispatchCommand(uint32_t cmd_id, uint32_t req_id,
                                      const uint8_t *payload, uint32_t payload_len)
{
    if (m_commandCallback)
        m_commandCallback(*this, cmd_id, req_id, payload, payload_len);
}

void SidebandSession::Close()
{
    // 先置断开标志，让后续的发送调用在拿锁之前就被挡掉
    m_connected.store(false, std::memory_order_release);

    {
        // 在发送锁内关闭 socket：否则可能在别的线程正处于 send() 之中时
        // 把句柄关掉并置为 INVALID_SOCKET。虽然结果只是 send 返回错误、
        // 被当作断开处理，但那是一次真实的数据竞争，没必要留着。
        std::lock_guard<std::mutex> lk(m_sendMutex);
        if (m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
        m_sendQueue.clear();
    }

    // 接收缓冲区只被主循环线程访问
    m_rxBuffer.clear();
}
