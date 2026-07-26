#include "SidebandServer.hpp"
#include "Logger.hpp"
#include "Json.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

namespace
{
// 单连接允许的认证失败次数，超过即断开（简单的暴力破解限速）
constexpr int kMaxAuthFailures = 5;

// 主循环等待事件的超时（毫秒）。也决定了 Tick 的抖动上限。
constexpr int kPollTimeoutMs = 15;
} // namespace

SidebandServer::SidebandServer()
    : m_listenSocket(INVALID_SOCKET), m_running(false), m_initialized(false)
{
}

SidebandServer::~SidebandServer()
{
    Shutdown();
}

bool SidebandServer::Initialize(uint16_t port, bool loopbackOnly)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Logger::Get().Error("WSAStartup 失败");
        return false;
    }
    m_initialized = true;

    m_listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET)
    {
        Logger::Get().Error("创建监听 socket 失败 err=", WSAGetLastError());
        return false;
    }

    // IPv4/IPv6 双栈
    int no = 0, yes = 1;
    setsockopt(m_listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&no, sizeof(no));

    // 注意：这里**不能**用 SO_REUSEADDR。
    // 它在 Windows 上的语义与 Linux 不同 —— 允许另一个进程绑定到已被占用的端口，
    // 从而劫持本服务的连接。正确做法是 SO_EXCLUSIVEADDRUSE 独占端口，
    // 顺带也让"重复启动第二份实例"这种情况能被 bind 明确拒绝。
    if (setsockopt(m_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (char *)&yes, sizeof(yes)) == SOCKET_ERROR)
    {
        Logger::Get().Warning("设置 SO_EXCLUSIVEADDRUSE 失败 err=", WSAGetLastError());
    }

    // 监听 socket 设为非阻塞
    u_long nonBlock = 1;
    ioctlsocket(m_listenSocket, FIONBIO, &nonBlock);

    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;

    if (bind(m_listenSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Logger::Get().Error("bind 失败, port=", port, " err=", WSAGetLastError(),
                            "（端口被占用？是否已有一份实例在运行）");
        return false;
    }
    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        Logger::Get().Error("listen 失败, err=", WSAGetLastError());
        return false;
    }

    Logger::Get().Info("SidebandServer: TCP 服务端已启动，端口=", port,
                       " 监听范围=", (loopbackOnly ? "仅本机" : "全部网卡"));
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

    std::list<SessionPtr> doomed;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        doomed.swap(m_clients);
    }
    // 在锁外关闭并通知，避免模块回调再次进入本对象时自锁
    for (auto &client : doomed)
    {
        client->Close();
        NotifyClientDisconnected(*client);
    }

    if (m_initialized)
    {
        WSACleanup();
        m_initialized = false;
    }
    Logger::Get().Info("SidebandServer: 已关闭");
}

void SidebandServer::SetAuthToken(std::string token)
{
    std::lock_guard<std::mutex> lock(m_authMutex);
    m_authToken = std::move(token);
}

void SidebandServer::RegisterModule(std::unique_ptr<ISidebandModule> module)
{
    if (module)
    {
        Logger::Get().Info("SidebandServer: 注册模块 ", module->GetName());
        m_modules.push_back(std::move(module));
    }
}

void SidebandServer::StartModules()
{
    if (m_modulesStarted)
        return;
    m_modulesStarted = true;

    for (auto &module : m_modules)
    {
        // 单个模块启动失败不影响其他模块（例如显示器控制不可用时，
        // 光标服务仍应继续工作）
        if (!module->Start())
            Logger::Get().Error("SidebandServer: 模块启动失败 ", module->GetName());
        else
            Logger::Get().Info("SidebandServer: 模块已启动 ", module->GetName());
    }
}

void SidebandServer::StopModules()
{
    if (!m_modulesStarted)
        return;
    m_modulesStarted = false;

    // 逆序停止
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
        (*it)->Stop();
}

void SidebandServer::NotifyDisplayChanged()
{
    for (auto &module : m_modules)
        module->OnDisplayChanged();
}

bool SidebandServer::HasClients() const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return !m_clients.empty();
}

size_t SidebandServer::GetClientCount() const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return m_clients.size();
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

// ============================================================
//                      广播
// ============================================================

void SidebandServer::BroadcastCursor(uint32_t hash, int32_t hotX, int32_t hotY,
                                     int32_t frames, int32_t delay,
                                     const std::vector<uint8_t> &pngData)
{
    std::vector<SessionPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.assign(m_clients.begin(), m_clients.end());
    }

    for (auto &client : snapshot)
    {
        if (client->IsConnected())
            client->SendCursor(hash, hotX, hotY, frames, delay, pngData);
    }
    // 断开的会话由主循环的 ReapDisconnected 统一摘除并通知，
    // 这里不在持锁状态下回调模块，避免潜在的自锁
}

void SidebandServer::BroadcastTextCursorState(int32_t yPercentage)
{
    std::vector<SessionPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.assign(m_clients.begin(), m_clients.end());
    }

    for (auto &client : snapshot)
    {
        if (client->IsConnected())
            client->SendTextCursorState(yPercentage);
    }
}

void SidebandServer::BroadcastCommand(uint32_t cmd_id, uint32_t req_id,
                                      const uint8_t *payload, uint32_t payload_len)
{
    std::vector<SessionPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.assign(m_clients.begin(), m_clients.end());
    }

    for (auto &client : snapshot)
    {
        if (client->IsConnected())
            client->SendCommand(cmd_id, req_id, payload, payload_len);
    }
}

// ============================================================
//                      指令路由与鉴权
// ============================================================

void SidebandServer::HandleAuth(SidebandSession &session, uint32_t req_id,
                                const uint8_t *payload, uint32_t payload_len)
{
    std::string payloadStr(payload ? (const char *)payload : "", payload ? payload_len : 0);
    std::string given = Json::GetString(payloadStr, "token");

    std::string expected;
    {
        std::lock_guard<std::mutex> lock(m_authMutex);
        expected = m_authToken;
    }

    std::string resp;
    if (expected.empty())
    {
        // 没有配置令牌时一律拒绝，避免"配置缺失=不设防"
        Logger::Get().Error("SidebandServer: 未配置控制令牌，拒绝认证请求");
        resp = R"({"ok":false,"error":"server_no_token"})";
    }
    else if (!given.empty() && given.size() == expected.size() &&
             // 定长比较，尽量不因提前返回泄露前缀信息
             [&]() {
                 unsigned char diff = 0;
                 for (size_t i = 0; i < expected.size(); ++i)
                     diff |= (unsigned char)(given[i] ^ expected[i]);
                 return diff == 0;
             }())
    {
        session.SetAuthenticated(true);
        Logger::Get().Info("SidebandServer: 客户端认证通过 ", session.PeerAddress());
        resp = R"({"ok":true})";
    }
    else
    {
        int fails = session.BumpAuthFailures();
        Logger::Get().Error("SidebandServer: 客户端认证失败(", fails, "/", kMaxAuthFailures,
                            ") ", session.PeerAddress());
        resp = R"({"ok":false,"error":"bad_token"})";

        if (fails >= kMaxAuthFailures)
        {
            std::vector<uint8_t> p(resp.begin(), resp.end());
            session.SendCommand(SidebandProtocol::Cmd::AUTH_RESP, req_id, p);
            Logger::Get().Error("SidebandServer: 认证失败次数超限，断开 ", session.PeerAddress());
            session.Close();
            return;
        }
    }

    std::vector<uint8_t> p(resp.begin(), resp.end());
    session.SendCommand(SidebandProtocol::Cmd::AUTH_RESP, req_id, p);
}

void SidebandServer::DispatchCommand(SidebandSession &session,
                                     uint32_t cmd_id, uint32_t req_id,
                                     const uint8_t *payload, uint32_t payload_len)
{
    using namespace SidebandProtocol;

    Logger::Get().Debug("SidebandServer: 收到指令 cmd=", cmd_id, " req=", req_id,
                        " len=", payload_len);

    // === 通用指令由服务器直接处理 ===
    switch (cmd_id)
    {
    case Cmd::HEARTBEAT:
        session.SendCommand(Cmd::HEARTBEAT, req_id, nullptr, 0);
        return;

    case Cmd::AUTH_REQ:
        HandleAuth(session, req_id, payload, payload_len);
        return;

    case Cmd::HELLO:
    {
        std::string resp = "{\"proto_ver\":" + std::to_string(PROTO_VERSION) +
                           ",\"caps\":[\"cursor\",\"display\"]"
                           ",\"auth_required_for\":[12,16,17]"
                           ",\"authenticated\":" + (session.IsAuthenticated() ? "true" : "false") +
                           "}";
        std::vector<uint8_t> p(resp.begin(), resp.end());
        session.SendCommand(Cmd::HELLO, req_id, p);
        return;
    }

    default:
        break;
    }

    // === 路由到唯一的处理模块 ===
    // 早期实现是把每条指令喂给所有模块、靠各模块 switch 的 default 忽略，
    // 模块一多就是 N×M 次无谓调用，而且 CursorModule 会看到所有显示器指令。
    ISidebandModule *handler = nullptr;
    for (auto &module : m_modules)
    {
        if (module->HandlesCommand(cmd_id))
        {
            handler = module.get();
            break;
        }
    }

    if (!handler)
    {
        Logger::Get().Debug("SidebandServer: 无人处理的 cmd_id=", cmd_id);
        return;
    }

    // === 分级授权：只有会改变系统状态的指令才要求认证 ===
    if (handler->CommandRequiresAuth(cmd_id) && !session.IsAuthenticated())
    {
        Logger::Get().Error("SidebandServer: 未认证的客户端尝试执行 cmd=", cmd_id,
                            " 来自 ", session.PeerAddress());

        // 用该请求本该收到的响应 cmd 回复，这样客户端按 reqId 挂起的请求能立即
        // 配对到一条 ok:false 的失败，而不是一直等到超时才报个笼统错误。
        // 模块没有声明固定响应 cmd 时才退回 AUTH_RESP。
        uint32_t respCmd = handler->ResponseCommandFor(cmd_id);
        if (respCmd == 0)
            respCmd = Cmd::AUTH_RESP;

        std::string resp = R"({"ok":false,"error":"unauthorized","cmd":)" +
                           std::to_string(cmd_id) + "}";
        std::vector<uint8_t> p(resp.begin(), resp.end());
        session.SendCommand(respCmd, req_id, p);
        return;
    }

    handler->OnCommand(session, cmd_id, req_id, payload, payload_len);
}

void SidebandServer::NotifyClientConnected(SidebandSession &session)
{
    for (auto &module : m_modules)
        module->OnClientConnected(session);
}

void SidebandServer::NotifyClientDisconnected(SidebandSession &session)
{
    Logger::Get().Info("SidebandServer: 客户端断开连接 ", session.PeerAddress());
    for (auto &module : m_modules)
        module->OnClientDisconnected(session);
}

void SidebandServer::TickModules()
{
    for (auto &module : m_modules)
        module->OnTick();
}

// ============================================================
//                      主循环
// ============================================================

void SidebandServer::AcceptNewClients()
{
    for (;;)
    {
        sockaddr_in6 clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSocket, (sockaddr *)&clientAddr, &addrLen);

        if (clientSock == INVALID_SOCKET)
        {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
                Logger::Get().Error("SidebandServer: accept 异常 err=", err);
            return;
        }

        char ipStr[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &clientAddr.sin6_addr, ipStr, INET6_ADDRSTRLEN);
        Logger::Get().Info("SidebandServer: 客户端已连接 ", ipStr);

        u_long nonBlock = 1;
        ioctlsocket(clientSock, FIONBIO, &nonBlock);

        auto session = std::make_shared<SidebandSession>(clientSock);
        session->SetPeerAddress(ipStr);
        session->SetCommandCallback(
            [this](SidebandSession &s, uint32_t cmd_id, uint32_t req_id,
                   const uint8_t *payload, uint32_t payload_len)
            { DispatchCommand(s, cmd_id, req_id, payload, payload_len); });

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.push_back(session);
        }

        // 在锁外通知模块，模块因此可以安全地回调 Broadcast*
        NotifyClientConnected(*session);
    }
}

void SidebandServer::ReapDisconnected()
{
    std::vector<SessionPtr> doomed;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto it = m_clients.begin(); it != m_clients.end();)
        {
            if (!(*it)->IsConnected())
            {
                doomed.push_back(*it);
                it = m_clients.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto &d : doomed)
    {
        d->Close();
        NotifyClientDisconnected(*d);
    }
}

void SidebandServer::Run()
{
    m_running = true;
    Logger::Get().Info("SidebandServer: 主循环开始");

    auto lastTick = std::chrono::steady_clock::now();
    constexpr auto kTickInterval = std::chrono::milliseconds(33); // ~30Hz

    std::vector<WSAPOLLFD> fds;
    std::vector<SessionPtr> polled;

    while (m_running)
    {
        // 组装本轮要等待的 socket 集合：[0] 是监听 socket，其后是各客户端
        fds.clear();
        polled.clear();

        WSAPOLLFD listenFd = {};
        listenFd.fd = m_listenSocket;
        listenFd.events = POLLRDNORM;
        fds.push_back(listenFd);

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            // polled 与 fds 必须在同一趟里同步构建：
            // 后面是用下标 fds[i+1] <-> polled[i] 对应的，分两趟过滤会错位。
            // 已关闭的会话其 socket 已是 INVALID_SOCKET，交给 WSAPoll 会让整次
            // 调用失败，所以在这里就跳过，留给 ReapDisconnected 摘除。
            for (auto &c : m_clients)
            {
                if (!c->IsConnected() || c->GetSocket() == INVALID_SOCKET)
                    continue;

                WSAPOLLFD pfd = {};
                pfd.fd = c->GetSocket();
                pfd.events = POLLRDNORM;
                // 只有还有积压数据时才关心可写事件，否则会被无休止唤醒
                if (c->HasQueuedData())
                    pfd.events |= POLLWRNORM;

                fds.push_back(pfd);
                polled.push_back(c);
            }
        }

        // 这里的超时同时充当 Tick 的节拍来源。
        // 早期实现是"忙轮询 + 没活干时 Sleep(2)"，而判断"有没有活干"的标志
        // 被无条件置真，结果只要有客户端连着就永远不休眠，空转吃满一个核。
        int ready = WSAPoll(fds.data(), (ULONG)fds.size(), kPollTimeoutMs);

        if (ready == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (m_running)
                Logger::Get().Error("SidebandServer: WSAPoll 失败 err=", err);
            Sleep(10);
        }
        else if (ready > 0)
        {
            // 新连接
            if (fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL))
                AcceptNewClients();

            // 客户端读写。fds[i+1] 与 polled[i] 一一对应。
            for (size_t i = 0; i < polled.size(); ++i)
            {
                auto &client = polled[i];
                const SHORT re = fds[i + 1].revents;
                if (re == 0)
                    continue;

                if (re & POLLWRNORM)
                    client->FlushSendQueue();

                if (re & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL))
                {
                    bool didWork = false;
                    client->TryReceive(didWork);
                }
            }
        }

        // 摘除断开的会话（可能由本轮 recv 判定，也可能由广播线程判定）
        ReapDisconnected();

        // 周期性 Tick
        auto now = std::chrono::steady_clock::now();
        if (now - lastTick >= kTickInterval)
        {
            TickModules();
            lastTick = now;
        }
    }

    Logger::Get().Info("SidebandServer: 主循环退出");
}
