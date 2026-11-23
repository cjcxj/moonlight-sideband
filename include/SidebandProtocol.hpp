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

// 单个包 Body 长度上限（防止恶意/损坏的长度前缀撑爆内存）
constexpr uint32_t MAX_BODY_LEN = 16u * 1024 * 1024;

// 单连接发送队列上限。客户端长时间不读时队列会堆积，
// 超过此值判定为客户端失速并断开，避免内存无限增长。
constexpr size_t MAX_SEND_QUEUE_BYTES = 4u * 1024 * 1024;

// 控制指令 CmdID 命名空间
namespace Cmd {
    // === 已有指令（向后兼容，老格式） ===
    // PC -> Android, payload: [YPercent(4)], 老格式固定 20 字节 body
    constexpr uint32_t TEXT_CURSOR_STATE = 2;

    // === 通用 ===
    constexpr uint32_t HEARTBEAT = 1;   // 双向, 空 payload（收到即原样回一条）
    constexpr uint32_t HELLO = 3;       // 双向, JSON: {"proto_ver":1,"caps":["cursor","display"]}

    // === 认证（分级授权）===
    // 光标推送、显示器列表/模式查询等只读操作**不需要**认证，
    // 因此老客户端无需任何改动即可继续工作。
    // 只有会改变系统状态的指令（DISPLAY_SWITCH / DISPLAY_MODE_SET / DISPLAY_SCALE_SET /
    // AUDIO_DEFAULT_SET / AUDIO_VOLUME_SET / AUDIO_MUTE_SET）
    // 要求该连接先通过 AUTH_REQ 提交令牌。令牌见 exe 同目录的 moonlight_sideband.ini。
    constexpr uint32_t AUTH_REQ = 4;    // Android -> PC, JSON: {"token":"..."}
    constexpr uint32_t AUTH_RESP = 5;   // PC -> Android, JSON: {"ok":true} 或 {"ok":false,"error":"bad_token"}

    // === Display 控制 (阶段 2 实现) ===
    constexpr uint32_t DISPLAY_LIST_REQ = 10;     // Android -> PC, 空                    [只读]
    constexpr uint32_t DISPLAY_LIST_RESP = 11;    // PC -> Android, JSON: [{id,name,w,h,is_primary},...]
    constexpr uint32_t DISPLAY_SWITCH = 12;       // Android -> PC, JSON: {display_id}     [需认证]
    constexpr uint32_t DISPLAY_CURRENT = 13;      // PC -> Android (事件通知), JSON: {display_id,w,h,refresh,scale}

    // === Display 模式 / 缩放修改 ===
    // 查询某显示器支持的所有显示模式
    constexpr uint32_t DISPLAY_MODE_LIST_REQ = 14;  // Android -> PC, JSON: {display_id}   [只读]
    constexpr uint32_t DISPLAY_MODE_LIST_RESP = 15; // PC -> Android, JSON: {display_id, modes:[{w,h,refresh,bpp},...]}
    // 设置分辨率/刷新率（立即生效）
    constexpr uint32_t DISPLAY_MODE_SET = 16;       // Android -> PC, JSON: {display_id,w,h,refresh}  [需认证]
    // 设置缩放（CCD API，即时生效）
    constexpr uint32_t DISPLAY_SCALE_SET = 17;      // Android -> PC, JSON: {display_id,scale}        [需认证]

    // === Audio 输出控制 (对应 Windows 设置里的"选择播放声音的位置") ===
    // 设备列表/音量/静音状态是只读查询；改默认设备、改音量、改静音需要认证。
    constexpr uint32_t AUDIO_LIST_REQ    = 30;  // Android -> PC, 空                        [只读]
    constexpr uint32_t AUDIO_LIST_RESP   = 31;  // PC -> Android, JSON: {devices:[...]}
    constexpr uint32_t AUDIO_DEFAULT_SET = 32;  // Android -> PC, JSON: {device_id}         [需认证]
    constexpr uint32_t AUDIO_VOLUME_SET  = 33;  // Android -> PC, JSON: {device_id,volume}  [需认证]
    constexpr uint32_t AUDIO_MUTE_SET    = 34;  // Android -> PC, JSON: {device_id,muted}   [需认证]
    constexpr uint32_t AUDIO_OP_RESP     = 35;  // PC -> Android, 上述三条写指令的统一响应
    constexpr uint32_t AUDIO_STATE       = 36;  // PC -> Android (事件推送), JSON 同 LIST_RESP

    // === Sunshine 配置 (阶段 3) ===
    constexpr uint32_t SUNSHINE_CONFIG_READ = 20;
    constexpr uint32_t SUNSHINE_CONFIG_RESP = 21;
    constexpr uint32_t SUNSHINE_CONFIG_WRITE = 22;
    constexpr uint32_t SUNSHINE_RESTART = 23;
    constexpr uint32_t SUNSHINE_STATUS = 24;

    // === 扩展区 ===
    constexpr uint32_t USER_RESERVED_START = 100;
    // 扩展文本光标状态推送（PC -> Android，仅对在 HELLO 中声明 "caret_ext":true 的客户端下发）
    // JSON: {"y":0..10000|-1,"x":0..10000|-1,"w":px,"h":px,"src":int,"conf":int,"comp":bool,"pwd":bool,"ctrl_bottom":0..10000|-1}
    constexpr uint32_t TEXT_CURSOR_EXT = 102;
}

// === 协议包构造辅助 ===

// 光标包头大小（BodyLen 之后的固定字段总长）
constexpr uint32_t CURSOR_HEADER_SIZE = 20;  // [Hash][HotX][HotY][Frames][Delay]

// 文本光标状态包（老格式，固定 24 字节）
constexpr uint32_t TEXT_CURSOR_PACKET_SIZE = 24;  // [BodyLen=20][Hash][CmdID=2][YPercent][Height][Source]

// 文本光标来源标记（BuildTextCursorPacket 的 sourceTag 参数 / 包尾保留字段 2）
// 兼容约定：0=无，1=Win32，2=MSAA/WinEvent，3=UIA；
// 4+ 为扩展来源（发给老客户端时按配置决定映射或降级）。
constexpr int32_t CARET_SOURCE_NONE        = 0;
constexpr int32_t CARET_SOURCE_WIN32       = 1;
constexpr int32_t CARET_SOURCE_MSAA        = 2;  // WinEvent OBJID_CARET / AccessibleObjectFromWindow
constexpr int32_t CARET_SOURCE_UIA         = 3;  // TextEditPattern / TextPattern2 / gated TextPattern
constexpr int32_t CARET_SOURCE_IA2         = 4;  // 预留：IAccessible2
constexpr int32_t CARET_SOURCE_IME_ANCHOR  = 5;  // IME 组合/候选窗口位置锚点兜底
constexpr int32_t CARET_SOURCE_CONTROL_BOX = 6;  // 仅输入控件矩形兜底（非精确插入符）
constexpr int32_t CARET_SOURCE_APP_MSG     = 7;  // Scintilla / Edit / RichEdit 直接窗口消息查询

// 文本光标置信度（用于区分精确插入符与区域估算）
enum class CaretConfidence : int32_t {
    None      = 0,
    Estimated = 1,  // IME 候选框 / 控件框估算
    Low       = 2,  // 启发式回退
    Medium    = 3,  // MSAA / 间接范围推算
    High      = 4,  // UIA 字符扩展 / 行尾借字符
    Exact     = 5   // Win32 / UIA GetCaretRange / Scintilla / EM_POSFROMCHAR 直接命中
};

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
//
// 老格式包末尾有两个历史保留字段（两个 [0]）。老客户端按 BodyLen=20
// 只解析到 YPercent，尾部字段被忽略；新客户端可继续读取：
//   - reserved1: 插入符高度（像素）。取 rcCaret.bottom - rcCaret.top。
//     语义是"输入行底边（YPercent 所指位置）再往上多少像素是行顶"，
//     下游"别让软键盘挡住当前输入行"应以底边 Y 为基准、避开高度。
//     0 = 未知（含状态为"无插入符"的包）。
//   - reserved2: 来源标记，见 CARET_SOURCE_*。
inline std::vector<uint8_t> BuildTextCursorPacket(int32_t yPercentage,
                                                  int32_t caretHeight = 0,
                                                  int32_t sourceTag = 0)
{
    uint32_t bodyLen = CURSOR_HEADER_SIZE;
    uint32_t magicHash = MAGIC_HASH;
    int32_t cmdId = 2;
    std::vector<uint8_t> packet(4 + bodyLen);
    uint8_t *p = packet.data();
    memcpy(p, &bodyLen, 4);     p += 4;
    memcpy(p, &magicHash, 4);  p += 4;
    memcpy(p, &cmdId, 4);      p += 4;
    memcpy(p, &yPercentage, 4); p += 4;
    memcpy(p, &caretHeight, 4); p += 4;
    memcpy(p, &sourceTag, 4);   p += 4;
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
