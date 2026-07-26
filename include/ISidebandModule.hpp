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
 *
 * 与早期版本的区别：Start()/Stop() 现在是接口的一部分。
 * 之前它们只存在于具体模块类上，导致 main.cpp 必须 include 每个模块的头文件、
 * 持有具体类型的裸指针并手动调用 —— 也就是说"注册新模块即可"其实做不到。
 * 现在 SidebandServer 统一管理模块生命周期，main.cpp 只剩一份注册列表。
 */
class ISidebandModule
{
public:
    virtual ~ISidebandModule() = default;

    // 模块名称（用于日志和调试）
    virtual const char *GetName() const = 0;

    // === 生命周期（由 SidebandServer 统一调用）===

    // 启动模块（拉起工作线程等）。返回 false 表示该模块不可用，
    // 但不影响其他模块继续运行。
    virtual bool Start() { return true; }

    // 停止模块并汇合其线程。必须可重入（多次调用安全）。
    virtual void Stop() {}

    // === 指令路由 ===

    // 本模块是否处理该指令。SidebandServer 依此把指令**只**投递给对应模块，
    // 而不是像以前那样广播给所有模块、靠各自 switch 的 default 忽略。
    virtual bool HandlesCommand(uint32_t /*cmd_id*/) const { return false; }

    // 该指令是否需要客户端已通过令牌认证。
    // 约定：只读查询返回 false；任何会改变系统状态的指令返回 true。
    // 这样老客户端（只收光标、不发指令）完全不受认证影响。
    virtual bool CommandRequiresAuth(uint32_t /*cmd_id*/) const { return false; }

    // 该请求指令对应的响应指令 ID。
    //
    // 用途：鉴权失败时服务器需要替模块回一条错误响应。如果统一回 AUTH_RESP，
    // 客户端那边按「reqId + 期望响应 cmd」配对的挂起请求就对不上号，只能干等到
    // 超时，用户看到的是笼统的"请求超时"而不是"未授权"。
    // 让模块声明"这条请求本该用哪个 cmd 回"，拒绝响应就能被正确配对，
    // 连未升级的老客户端也会立刻弹出可读的错误。
    //
    // 返回 0 表示没有固定响应指令，此时服务器回 AUTH_RESP。
    virtual uint32_t ResponseCommandFor(uint32_t /*cmd_id*/) const { return 0; }

    // === 回调 ===

    // 客户端连接/断开。注意：由 SidebandServer 在**未持有客户端列表锁**时调用，
    // 因此实现里可以安全地回调 Broadcast* 系列方法。
    virtual void OnClientConnected(SidebandSession & /*session*/) {}
    virtual void OnClientDisconnected(SidebandSession & /*session*/) {}

    // 处理来自客户端的控制指令
    // cmd_id: 指令 ID（参见 SidebandProtocol::Cmd）
    // req_id: 请求 ID（客户端生成，用于匹配请求/响应）
    // payload / payload_len: 指令负载
    virtual void OnCommand(SidebandSession & /*session*/,
                           uint32_t /*cmd_id*/,
                           uint32_t /*req_id*/,
                           const uint8_t * /*payload*/,
                           uint32_t /*payload_len*/) {}

    // 周期性任务（约 30Hz）
    virtual void OnTick() {}

    // 系统显示配置发生变化（由主窗口的 WM_DISPLAYCHANGE 触发）。
    // 在 UI 线程上调用，实现里只应置标志/唤醒工作线程，不要做耗时操作。
    virtual void OnDisplayChanged() {}
};
