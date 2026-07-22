#include "SidebandServer.hpp"
#include "Logger.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

SidebandServer::SidebandServer()
    : m_listenSocket(INVALID_SOCKET), m_running(false), m_initialized(false)
{
}

SidebandServer::~SidebandServer()
{
    Shutdown();
}

bool SidebandServer::Initialize(uint16_t port)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Logger::Get().Error("WSAStartup 失败");
        return false;
    }

    m_listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET)
    {
        Logger::Get().Error("创建监听 socket 失败");
        return false;
    }

    // IPv4/IPv6 双栈
    int no = 0, yes = 1;
    setsockopt(m_listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&no, sizeof(no));
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    // 监听 socket 设为非阻塞，便于主循环轮询
    u_long nonBlock = 1;
    ioctlsocket(m_listenSocket, FIONBIO, &nonBlock);

    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;

    if (bind(m_listenSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Logger::Get().Error("bind 失败, port=", port, " err=", WSAGetLastError());
        return false;
    }
    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        Logger::Get().Error("listen 失败, err=", WSAGetLastError());
        return false;
    }

    m_initialized = true;
    Logger::Get().Info("SidebandServer: TCP 服务端已启动，端口=", port);
    return true;
}

void SidebandServer::Shutdown()
{
    m_running = false;

    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto &client : m_clients)
            client->Close();
        m_clients.clear();
    }

    if (m_initialized)
    {
        WSACleanup();
        m_initialized = false;
    }
    Logger::Get().Info("SidebandServer: 已关闭");
}

void SidebandServer::RegisterModule(std::unique_ptr<ISidebandModule> module)
{
    if (module)
    {
        Logger::Get().Info("SidebandServer: 注册模块 ", module->GetName());
        m_modules.push_back(std::move(module));
    }
}

bool SidebandServer::HasClients() const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return !m_clients.empty();
}

bool SidebandServer::GetCachedPng(uint32_t hash, std::vector<uint8_t> &outPng)
{
    std::lock_guard<std::mutex> lock(m_pngCacheMutex);
    auto it = m_pngCache.find(hash);
    if (it != m_pngCache.end())
    {
        outPng = it->second;
        return true;
    }
    return false;
}

void SidebandServer::CachePng(uint32_t hash, const std::vector<uint8_t> &pngData)
{
    std::lock_guard<std::mutex> lock(m_pngCacheMutex);
    if (m_pngCache.size() > 50)
        m_pngCache.clear();
    m_pngCache[hash] = pngData;
}

void SidebandServer::BroadcastCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                                     int32_t frames, int32_t delay,
                                     const std::vector<uint8_t> &pngData)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    for (auto it = m_clients.begin(); it != m_clients.end();)
    {
        auto &client = *it;
        if (!client->IsConnected())
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }

        // 连续去重
        if (client->LastSentHash() == hash)
        {
            ++it;
            continue;
        }

        // 缓存检查
        bool isCacheHit = client->CachedHashes().find(hash) != client->CachedHashes().end();
        if (!isCacheHit)
        {
            // 全量包，记录缓存
            client->CachedHashes().insert(hash);
            if (client->CachedHashes().size() > 100)
                client->CachedHashes().clear();
        }

        if (!client->SendCursor(hash, hotX, hotY, frames, delay, pngData, isCacheHit))
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }
        ++it;
    }
}

void SidebandServer::BroadcastTextCursorState(int32_t yPercentage)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto it = m_clients.begin(); it != m_clients.end();)
    {
        auto &client = *it;
        if (!client->IsConnected())
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }
        if (!client->SendTextCursorState(yPercentage))
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }
        ++it;
    }
}

void SidebandServer::BroadcastCommand(uint32_t cmd_id, uint32_t req_id,
                                      const uint8_t *payload, uint32_t payload_len)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto it = m_clients.begin(); it != m_clients.end();)
    {
        auto &client = *it;
        if (!client->IsConnected())
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }
        if (!client->SendCommand(cmd_id, req_id, payload, payload_len))
        {
            NotifyClientDisconnected(*client);
            it = m_clients.erase(it);
            continue;
        }
        ++it;
    }
}

void SidebandServer::DispatchCommand(SidebandSession &session,
                                     uint32_t cmd_id, uint32_t req_id,
                                     const uint8_t *payload, uint32_t payload_len)
{
    Logger::Get().Debug("SidebandServer: 收到指令 cmd=", cmd_id, " req=", req_id,
                        " len=", payload_len);
    // 分发给所有模块
    for (auto &module : m_modules)
    {
        module->OnCommand(session, cmd_id, req_id, payload, payload_len);
    }
}

void SidebandServer::NotifyClientConnected(SidebandSession &session)
{
    for (auto &module : m_modules)
        module->OnClientConnected(session);
}

void SidebandServer::NotifyClientDisconnected(SidebandSession &session)
{
    Logger::Get().Info("SidebandServer: 客户端断开连接");
    for (auto &module : m_modules)
        module->OnClientDisconnected(session);
}

void SidebandServer::TickModules()
{
    for (auto &module : m_modules)
        module->OnTick();
}

void SidebandServer::SetupSessionCallbacks(std::shared_ptr<SidebandSession> &session)
{
    // 捕获 this 不可见地共享生命周期；这里 server 比所有 session 长寿
    SidebandServer *self = this;
    session->SetCommandCallback([self](SidebandSession &s, uint32_t cmd_id, uint32_t req_id,
                                      const uint8_t *payload, uint32_t payload_len)
                                { self->DispatchCommand(s, cmd_id, req_id, payload, payload_len); });
}

void SidebandServer::Run()
{
    m_running = true;
    Logger::Get().Info("SidebandServer: 主循环开始");

    auto lastTick = std::chrono::steady_clock::now();
    constexpr auto kTickInterval = std::chrono::milliseconds(33); // ~30Hz

    while (m_running)
    {
        bool didWork = false;

        // 1. 尝试接受新连接（非阻塞）
        sockaddr_in6 clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSocket, (sockaddr *)&clientAddr, &addrLen);
        if (clientSock != INVALID_SOCKET)
        {
            didWork = true;
            char ipStr[INET6_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET6, &clientAddr.sin6_addr, ipStr, INET6_ADDRSTRLEN);
            Logger::Get().Info("SidebandServer: 客户端已连接 ", ipStr);

            // 设为非阻塞
            u_long nonBlock = 1;
            ioctlsocket(clientSock, FIONBIO, &nonBlock);

            auto session = std::make_shared<SidebandSession>(clientSock);
            SetupSessionCallbacks(session);

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                m_clients.push_back(session);
            }
            NotifyClientConnected(*session);
        }
        else
        {
            // EWOULDBLOCK 是正常的（没新连接）
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                Logger::Get().Error("SidebandServer: accept 异常 err=", err);
            }
        }

        // 2. 轮询所有客户端 socket，接收数据
        //    在锁内拷贝客户端列表（shared_ptr 保持引用），在锁外调用 TryReceive
        //    避免持锁期间 OnCommand 执行耗时操作（ChangeDisplaySettings 等）阻塞其他线程
        std::vector<std::shared_ptr<SidebandSession>> clientsSnapshot;
        std::vector<std::shared_ptr<SidebandSession>> disconnected;

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (auto &c : m_clients)
            {
                c->FlushSendQueue();
                clientsSnapshot.push_back(c);
            }
        }

        // 锁外接收并处理指令（OnCommand 在此执行，可能耗时）
        for (auto &client : clientsSnapshot)
        {
            if (!client->TryReceive())
                disconnected.push_back(client);
            didWork = true;
        }

        // 锁内移除断开的客户端
        if (!disconnected.empty())
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (auto &d : disconnected)
            {
                m_clients.remove(d);
                NotifyClientDisconnected(*d);
            }
        }

        // 3. 周期性 Tick
        auto now = std::chrono::steady_clock::now();
        if (now - lastTick >= kTickInterval)
        {
            TickModules();
            lastTick = now;
            didWork = true;
        }

        // 4. 没事做时短暂休息，避免 100% CPU
        if (!didWork)
        {
            Sleep(2);
        }
    }

    Logger::Get().Info("SidebandServer: 主循环退出");
}
