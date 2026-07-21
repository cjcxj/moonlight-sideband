#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace SidebandProtocol {

// 特殊 Hash 值，标记控制指令包
constexpr uint32_t MAGIC_HASH = 0xFFFFFFFF;

// 默认端口（与原 windows-cursor-streamer 一致）
constexpr uint16_t DEFAULT_PORT = 5005;

// 协议版本
constexpr uint32_t PROTO_VERSION = 1;

// 控制指令 CmdID 命名空间
namespace Cmd {
    // === 已有指令（向后兼容，老格式） ===
    // PC -> Android, payload: [YPercent(4)], 老格式固定 20 字节 body
    constexpr uint32_t TEXT_CURSOR_STATE = 2;

    // === 通用 ===
    constexpr uint32_t HEARTBEAT = 1;   // 双向, 空 payload
    constexpr uint32_t HELLO = 3;       // 双向, JSON: {"proto_ver":1,"caps":["cursor","display"]}

    // === Display 控制 (阶段 2 实现) ===
    constexpr uint32_t DISPLAY_LIST_REQ = 10;     // Android -> PC, 空
    constexpr uint32_t DISPLAY_LIST_RESP = 11;    // PC -> Android, JSON: [{id,name,w,h,is_primary},...]
    constexpr uint32_t DISPLAY_SWITCH = 12;       // Android -> PC, JSON: {display_id}
    constexpr uint32_t DISPLAY_CURRENT = 13;      // PC -> Android (事件通知), JSON: {display_id,w,h,refresh,scale}

    // === Display 模式 / 缩放修改 ===
    // 查询某显示器支持的所有显示模式
    constexpr uint32_t DISPLAY_MODE_LIST_REQ = 14;  // Android -> PC, JSON: {display_id}
    constexpr uint32_t DISPLAY_MODE_LIST_RESP = 15; // PC -> Android, JSON: {display_id, modes:[{w,h,refresh,bpp},...]}
    // 设置分辨率/刷新率（立即生效）
    constexpr uint32_t DISPLAY_MODE_SET = 16;       // Android -> PC, JSON: {display_id,w,h,refresh}
    // 设置缩放（写注册表，需要注销/登录生效）
    constexpr uint32_t DISPLAY_SCALE_SET = 17;      // Android -> PC, JSON: {display_id,scale}

    // === Sunshine 配置 (阶段 3) ===
    constexpr uint32_t SUNSHINE_CONFIG_READ = 20;
    constexpr uint32_t SUNSHINE_CONFIG_RESP = 21;
    constexpr uint32_t SUNSHINE_CONFIG_WRITE = 22;
    constexpr uint32_t SUNSHINE_RESTART = 23;
    constexpr uint32_t SUNSHINE_STATUS = 24;

    // === 扩展区 ===
    constexpr uint32_t USER_RESERVED_START = 100;
}

// === 协议包构造辅助 ===

// 光标包头大小（BodyLen 之后的固定字段总长）
constexpr uint32_t CURSOR_HEADER_SIZE = 20;  // [Hash][HotX][HotY][Frames][Delay]

// 文本光标状态包（老格式，固定 24 字节）
constexpr uint32_t TEXT_CURSOR_PACKET_SIZE = 24;  // [BodyLen=20][Hash][CmdID=2][YPercent][0][0]

// 新控制指令包头大小（BodyLen 之后）
constexpr uint32_t COMMAND_HEADER_SIZE = 16;  // [Hash][CmdID][ReqID][PayloadLen]

// 构造光标包（兼容老协议）
inline std::vector<uint8_t> BuildCursorPacket(uint32_t hash, int32_t hotX, int32_t hotY,
                                              int32_t frames, int32_t delay,
                                              const std::vector<uint8_t> &pngData)
{
    uint32_t bodyLen = CURSOR_HEADER_SIZE + (uint32_t)pngData.size();
    std::vector<uint8_t> packet(4 + bodyLen);
    uint8_t *p = packet.data();
    memcpy(p, &bodyLen, 4); p += 4;
    memcpy(p, &hash, 4);    p += 4;
    memcpy(p, &hotX, 4);    p += 4;
    memcpy(p, &hotY, 4);    p += 4;
    memcpy(p, &frames, 4);  p += 4;
    memcpy(p, &delay, 4);   p += 4;
    if (!pngData.empty())
        memcpy(p, pngData.data(), pngData.size());
    return packet;
}

// 构造短光标包（缓存命中，仅 Header）
inline std::vector<uint8_t> BuildCachedCursorPacket(uint32_t hash, int32_t hotX, int32_t hotY,
                                                    int32_t frames, int32_t delay)
{
    uint32_t bodyLen = CURSOR_HEADER_SIZE;
    std::vector<uint8_t> packet(4 + bodyLen);
    uint8_t *p = packet.data();
    memcpy(p, &bodyLen, 4); p += 4;
    memcpy(p, &hash, 4);    p += 4;
    memcpy(p, &hotX, 4);    p += 4;
    memcpy(p, &hotY, 4);    p += 4;
    memcpy(p, &frames, 4);  p += 4;
    memcpy(p, &delay, 4);   p += 4;
    return packet;
}

// 构造文本光标状态包（老格式, CmdID=2）
inline std::vector<uint8_t> BuildTextCursorPacket(int32_t yPercentage)
{
    uint32_t bodyLen = CURSOR_HEADER_SIZE;
    uint32_t magicHash = MAGIC_HASH;
    int32_t cmdId = 2;
    int32_t zero = 0;
    std::vector<uint8_t> packet(4 + bodyLen);
    uint8_t *p = packet.data();
    memcpy(p, &bodyLen, 4);   p += 4;
    memcpy(p, &magicHash, 4); p += 4;
    memcpy(p, &cmdId, 4);     p += 4;
    memcpy(p, &yPercentage, 4); p += 4;
    memcpy(p, &zero, 4);      p += 4;
    memcpy(p, &zero, 4);      p += 4;
    return packet;
}

// 构造新控制指令包
inline std::vector<uint8_t> BuildCommandPacket(uint32_t cmd_id, uint32_t req_id,
                                               const uint8_t *payload, uint32_t payload_len)
{
    uint32_t bodyLen = COMMAND_HEADER_SIZE + payload_len;
    uint32_t magicHash = MAGIC_HASH;
    std::vector<uint8_t> packet(4 + bodyLen);
    uint8_t *p = packet.data();
    memcpy(p, &bodyLen, 4);    p += 4;
    memcpy(p, &magicHash, 4);  p += 4;
    memcpy(p, &cmd_id, 4);     p += 4;
    memcpy(p, &req_id, 4);     p += 4;
    memcpy(p, &payload_len, 4); p += 4;
    if (payload_len > 0 && payload)
        memcpy(p, payload, payload_len);
    return packet;
}

inline std::vector<uint8_t> BuildCommandPacket(uint32_t cmd_id, uint32_t req_id,
                                               const std::vector<uint8_t> &payload)
{
    return BuildCommandPacket(cmd_id, req_id, payload.data(), (uint32_t)payload.size());
}

} // namespace SidebandProtocol
