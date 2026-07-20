#include "SidebandSession.hpp"
#include "Logger.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <iostream>

SidebandSession::SidebandSession(SOCKET sock)
    : m_socket(sock), m_connected(true), m_lastSentHash(0)
{
    // 禁用 Nagle 算法，低延迟
    int yes = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&yes, sizeof(yes));
}

SidebandSession::~SidebandSession()
{
    Close();
}

bool SidebandSession::SendCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                                 int32_t frames, int32_t delay,
                                 const std::vector<uint8_t> &pngData, bool isCacheHit)
{
    if (!m_connected)
        return false;

    std::vector<uint8_t> packet = isCacheHit
        ? SidebandProtocol::BuildCachedCursorPacket(hash, hotX, hotY, frames, delay)
        : SidebandProtocol::BuildCursorPacket(hash, hotX, hotY, frames, delay, pngData);

    int sent = send(m_socket, (const char *)packet.data(), (int)packet.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        Logger::Get().Debug("SidebandSession: send 失败 (cursor)");
        m_connected = false;
        return false;
    }
    m_lastSentHash = hash;
    return true;
}

bool SidebandSession::SendTextCursorState(int32_t yPercentage)
{
    if (!m_connected)
        return false;
    auto packet = SidebandProtocol::BuildTextCursorPacket(yPercentage);
    int sent = send(m_socket, (const char *)packet.data(), (int)packet.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        Logger::Get().Debug("SidebandSession: send 失败 (text-cursor)");
        m_connected = false;
        return false;
    }
    return true;
}

bool SidebandSession::SendCommand(uint32_t cmd_id, uint32_t req_id,
                                  const uint8_t *payload, uint32_t payload_len)
{
    if (!m_connected)
        return false;
    auto packet = SidebandProtocol::BuildCommandPacket(cmd_id, req_id, payload, payload_len);
    int sent = send(m_socket, (const char *)packet.data(), (int)packet.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        Logger::Get().Debug("SidebandSession: send 失败 (command)");
        m_connected = false;
        return false;
    }
    return true;
}

bool SidebandSession::TryReceive()
{
    if (!m_connected)
        return false;

    // 读取可用数据
    char buf[4096];
    int received = recv(m_socket, buf, sizeof(buf), 0);

    if (received == 0)
    {
        // 客户端正常关闭
        m_connected = false;
        return false;
    }
    if (received == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            // 非阻塞 socket，没数据可读，正常
            return true;
        }
        // 真正的错误
        Logger::Get().Debug("SidebandSession: recv 失败 WSA=", err);
        m_connected = false;
        return false;
    }

    // 追加到缓冲区
    m_rxBuffer.insert(m_rxBuffer.end(), buf, buf + received);
    ProcessRxBuffer();
    return true;
}

void SidebandSession::ProcessRxBuffer()
{
    // 协议格式:
    //   [BodyLen(4)] [Body(BodyLen)]
    // Body 内部:
    //   [Hash(4)] [CmdID(4)] [ReqID(4)] [PayloadLen(4)] [Payload...]
    //   (Hash == 0xFFFFFFFF)

    while (m_rxBuffer.size() >= 4)
    {
        uint32_t bodyLen;
        memcpy(&bodyLen, m_rxBuffer.data(), 4);

        // 合理性检查：避免恶意大包导致内存爆炸
        if (bodyLen > 16 * 1024 * 1024) // 16 MB 上限
        {
            Logger::Get().Error("SidebandSession: 收到超大包 BodyLen=", bodyLen, "，断开连接");
            m_connected = false;
            m_rxBuffer.clear();
            return;
        }

        // 数据还没收齐
        if (m_rxBuffer.size() < 4 + bodyLen)
            return;

        const uint8_t *body = m_rxBuffer.data() + 4;

        // 必须是控制指令包（Hash == MAGIC_HASH）
        // 客户端发到 PC 的只允许是控制指令
        if (bodyLen < 4)
        {
            Logger::Get().Info("SidebandSession: BodyLen < 4，跳过");
            m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + 4 + bodyLen);
            continue;
        }

        uint32_t hash;
        memcpy(&hash, body, 4);

        if (hash != SidebandProtocol::MAGIC_HASH)
        {
            // 老协议下客户端不会发数据到 PC；忽略此包
            Logger::Get().Debug("SidebandSession: 收到非控制包 hash=", hash, "，忽略");
            m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + 4 + bodyLen);
            continue;
        }

        // 控制指令包: [Hash][CmdID][ReqID][PayloadLen][Payload...]
        if (bodyLen < SidebandProtocol::COMMAND_HEADER_SIZE)
        {
            Logger::Get().Info("SidebandSession: 控制包头不足, BodyLen=", bodyLen);
            m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + 4 + bodyLen);
            continue;
        }

        uint32_t cmd_id, req_id, payload_len;
        memcpy(&cmd_id, body + 4, 4);
        memcpy(&req_id, body + 8, 4);
        memcpy(&payload_len, body + 12, 4);

        // payload 长度校验
        if (bodyLen != SidebandProtocol::COMMAND_HEADER_SIZE + payload_len)
        {
            Logger::Get().Info("SidebandSession: PayloadLen 不匹配, declared=",
                               payload_len, " actual=", bodyLen - SidebandProtocol::COMMAND_HEADER_SIZE);
            m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + 4 + bodyLen);
            continue;
        }

        const uint8_t *payload = (payload_len > 0) ? (body + 16) : nullptr;
        DispatchCommand(cmd_id, req_id, payload, payload_len);

        // 移除已处理包
        m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + 4 + bodyLen);
    }
}

void SidebandSession::DispatchCommand(uint32_t cmd_id, uint32_t req_id,
                                      const uint8_t *payload, uint32_t payload_len)
{
    if (m_commandCallback)
    {
        m_commandCallback(*this, cmd_id, req_id, payload, payload_len);
    }
}

void SidebandSession::Close()
{
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_connected = false;
    m_rxBuffer.clear();
}
