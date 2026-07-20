#pragma once

#include <cstdint>
#include <vector>

// 前向声明
class SidebandSession;

/**
 * 旁路服务模块接口
 *
 * 模块通过实现此接口响应客户端指令或周期性任务。
 * 模块不应直接操作 socket，应通过 SidebandSession 提供的方法发送数据。
 */
class ISidebandModule
{
public:
    virtual ~ISidebandModule() = default;

    // 模块名称（用于日志和调试）
    virtual const char *GetName() const = 0;

    // 客户端连接/断开回调
    virtual void OnClientConnected(SidebandSession &session) {}
    virtual void OnClientDisconnected(SidebandSession &session) {}

    // 处理来自客户端的控制指令
    // cmd_id: 指令 ID（参见 SidebandProtocol::Cmd）
    // req_id: 请求 ID（客户端生成，用于匹配请求/响应）
    // payload / payload_len: 指令负载
    virtual void OnCommand(SidebandSession &session,
                           uint32_t cmd_id,
                           uint32_t req_id,
                           const uint8_t *payload,
                           uint32_t payload_len) {}

    // 周期性任务（约 30Hz）
    virtual void OnTick() {}
};
