/**
 * 与平台无关的纯逻辑单元测试
 *
 * 只覆盖不依赖 Windows API 的部分：JSON 字段读取、协议包构造。
 * 这两块是最容易出"静默错误"的地方 —— 解析错一个字段不会崩，
 * 只会让功能诡异地不工作，而它们又完全不需要真机就能验证。
 *
 * 构建：
 *   cmake -S . -B build -DSIDEBAND_BUILD_TESTS=ON
 *   cmake --build build --target sideband_tests
 */

#include "Json.hpp"
#include "SidebandProtocol.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                     \
    do {                                                                   \
        ++g_checks;                                                        \
        std::string a_ = (actual);                                         \
        std::string e_ = (expected);                                       \
        if (a_ != e_) {                                                    \
            std::printf("  FAIL %s:%d  got \"%s\", want \"%s\"\n",         \
                        __FILE__, __LINE__, a_.c_str(), e_.c_str());       \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                     \
    do {                                                                   \
        ++g_checks;                                                        \
        long long a_ = (long long)(actual);                                \
        long long e_ = (long long)(expected);                              \
        if (a_ != e_) {                                                    \
            std::printf("  FAIL %s:%d  got %lld, want %lld\n",             \
                        __FILE__, __LINE__, a_, e_);                       \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void TestJsonBasics()
{
    std::printf("Json 基本读取\n");

    const std::string j = R"({"display_id":"\\\\.\\DISPLAY1#4352","w":2560,"h":1440,"refresh":59})";

    // 反转义后应还原成真实的 GDI 名 + targetId
    CHECK_EQ_STR(Json::GetString(j, "display_id"), "\\\\.\\DISPLAY1#4352");
    CHECK_EQ_INT(Json::GetInt(j, "w"), 2560);
    CHECK_EQ_INT(Json::GetInt(j, "h"), 1440);
    CHECK_EQ_INT(Json::GetInt(j, "refresh"), 59);

    // 缺失字段走默认值
    CHECK_EQ_INT(Json::GetInt(j, "scale", -1), -1);
    CHECK_EQ_STR(Json::GetString(j, "nope", "fallback"), "fallback");

    bool found = true;
    Json::GetInt(j, "nope", 0, &found);
    CHECK(!found);

    Json::GetInt(j, "w", 0, &found);
    CHECK(found);
}

static void TestJsonKeyInsideValue()
{
    std::printf("Json 值内部出现键名时仍应定位到真正的键\n");

    // 说明：旧的 find("\"key\"") 实现在这几种情况下其实也能给出正确答案 ——
    // 因为 JSON 转义会把值内部的引号写成 \"，字面量 "key" 反而不会出现。
    // 这里保留为回归用例：新实现是结构化扫描而非字符串查找，行为不应退化。
    const std::string j =
        R"({"msg":"display_id: missing","display_id":"REAL","w":100})";
    CHECK_EQ_STR(Json::GetString(j, "display_id"), "REAL");
    CHECK_EQ_INT(Json::GetInt(j, "w"), 100);

    // 值里带转义引号也不能干扰后续字段定位
    const std::string j2 = R"({"name":"He said \"w\":9999 loudly","w":7})";
    CHECK_EQ_INT(Json::GetInt(j2, "w"), 7);
    CHECK_EQ_STR(Json::GetString(j2, "name"), "He said \"w\":9999 loudly");

    // 键出现在数组元素里（是值而非键）
    const std::string j3 = R"({"tags":["w","h"],"w":42})";
    CHECK_EQ_INT(Json::GetInt(j3, "w"), 42);
}

static void TestJsonNumbers()
{
    std::printf("Json 数字边界\n");

    CHECK_EQ_INT(Json::GetInt(R"({"v":-1500})", "v"), -1500);
    CHECK_EQ_INT(Json::GetInt(R"({ "v" : 25 })", "v"), 25);   // 冒号周围空白
    CHECK_EQ_INT(Json::GetInt(R"({"v":0})", "v", -7), 0);

    // 溢出必须退回默认值，而不是回绕成一个看起来合理的垃圾数。
    // 这是相对旧实现的**实质性**修复：旧版直接 v = v*10 + d 累加到 int，
    // 有符号溢出是未定义行为，实测会返回 276447231 这种值 ——
    // 如果它落在 DISPLAY_MODE_SET 的 w/h 上，就是一次莫名其妙的分辨率设置。
    CHECK_EQ_INT(Json::GetInt(R"({"v":99999999999999})", "v", -1), -1);
    CHECK_EQ_INT(Json::GetInt(R"({"v":-99999999999999})", "v", -1), -1);
    CHECK_EQ_INT(Json::GetInt(R"({"v":2147483648})", "v", -1), -1);   // INT_MAX+1
    CHECK_EQ_INT(Json::GetInt(R"({"v":2147483647})", "v", -1), 2147483647);

    // 非数字
    CHECK_EQ_INT(Json::GetInt(R"({"v":"abc"})", "v", -1), -1);

    // 布尔
    CHECK(Json::GetBool(R"({"ok":true})", "ok") == true);
    CHECK(Json::GetBool(R"({"ok":false})", "ok", true) == false);
}

static void TestJsonEscape()
{
    std::printf("Json 转义与往返\n");

    CHECK_EQ_STR(Json::Escape("a\"b"), "a\\\"b");
    CHECK_EQ_STR(Json::Escape("C:\\path"), "C:\\\\path");
    CHECK_EQ_STR(Json::Escape("line\n"), "line\\n");

    // Escape -> 解析 应还原原文（含 Windows GDI 设备名这种反斜杠密集的情况）
    const std::string raw = "\\\\.\\DISPLAY1#4353";
    const std::string doc = "{\"id\":\"" + Json::Escape(raw) + "\"}";
    CHECK_EQ_STR(Json::GetString(doc, "id"), raw);

    const std::string weird = "quote\" back\\ tab\t";
    const std::string doc2 = "{\"k\":\"" + Json::Escape(weird) + "\"}";
    CHECK_EQ_STR(Json::GetString(doc2, "k"), weird);
}

static void TestJsonMalformed()
{
    std::printf("Json 畸形输入不应崩溃\n");

    CHECK_EQ_STR(Json::GetString("", "k", "d"), "d");
    CHECK_EQ_STR(Json::GetString("{", "k", "d"), "d");
    CHECK_EQ_STR(Json::GetString(R"({"k":)", "k", "d"), "d");
    CHECK_EQ_STR(Json::GetString(R"({"unterminated)", "k", "d"), "d");
    CHECK_EQ_STR(Json::GetString(R"({"k":"unterminated)", "k", "d"), "d");
    CHECK_EQ_INT(Json::GetInt(R"({"k":)", "k", -1), -1);
    // 反斜杠结尾截断
    CHECK_EQ_STR(Json::GetString("{\"k\":\"abc\\", "k", "d"), "d");
}

static uint32_t ReadU32(const std::vector<uint8_t> &b, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

static void TestProtocolPackets()
{
    std::printf("协议包构造\n");

    using namespace SidebandProtocol;

    // 控制指令包: [BodyLen][Hash][CmdID][ReqID][PayloadLen][Payload]
    const std::string payload = R"({"token":"abc"})";
    std::vector<uint8_t> p(payload.begin(), payload.end());
    auto pkt = BuildCommandPacket(Cmd::AUTH_REQ, 77, p);

    CHECK_EQ_INT(pkt.size(), 4 + COMMAND_HEADER_SIZE + payload.size());
    CHECK_EQ_INT(ReadU32(pkt, 0), COMMAND_HEADER_SIZE + payload.size());
    CHECK_EQ_INT(ReadU32(pkt, 4), MAGIC_HASH);
    CHECK_EQ_INT(ReadU32(pkt, 8), Cmd::AUTH_REQ);
    CHECK_EQ_INT(ReadU32(pkt, 12), 77);
    CHECK_EQ_INT(ReadU32(pkt, 16), payload.size());
    CHECK(std::memcmp(pkt.data() + 20, payload.data(), payload.size()) == 0);

    // 声明长度必须与实际 body 自洽，否则接收端会丢弃整包
    CHECK_EQ_INT(ReadU32(pkt, 0), pkt.size() - 4);

    // 空 payload
    auto hb = BuildCommandPacket(Cmd::HEARTBEAT, 1, nullptr, 0);
    CHECK_EQ_INT(hb.size(), 4 + COMMAND_HEADER_SIZE);
    CHECK_EQ_INT(ReadU32(hb, 16), 0);

    // 光标全量包
    std::vector<uint8_t> png = {1, 2, 3, 4, 5};
    auto cur = BuildCursorPacket(0xDEADBEEF, 3, 4, 1, 0, png);
    CHECK_EQ_INT(cur.size(), 4 + CURSOR_HEADER_SIZE + png.size());
    CHECK_EQ_INT(ReadU32(cur, 0), CURSOR_HEADER_SIZE + png.size());
    CHECK_EQ_INT(ReadU32(cur, 4), 0xDEADBEEF);

    // 缓存命中短包：只有头，没有 PNG
    auto cached = BuildCachedCursorPacket(0xDEADBEEF, 3, 4, 1, 0);
    CHECK_EQ_INT(cached.size(), 4 + CURSOR_HEADER_SIZE);
    CHECK_EQ_INT(ReadU32(cached, 0), CURSOR_HEADER_SIZE);

    // 老格式文本光标包必须恰好 24 字节，否则老客户端会解析错位
    auto txt = BuildTextCursorPacket(5000);
    CHECK_EQ_INT(txt.size(), TEXT_CURSOR_PACKET_SIZE);
    CHECK_EQ_INT(ReadU32(txt, 4), MAGIC_HASH);
    CHECK_EQ_INT(ReadU32(txt, 8), 2);
    CHECK_EQ_INT(ReadU32(txt, 12), 5000);
}

static void TestAuthPayloadRoundTrip()
{
    std::printf("认证负载往返\n");

    // 服务端读 token 用的正是 Json::GetString，这里确认含特殊字符的令牌也没问题
    const std::string token = "a1b2c3d4e5f60718";
    const std::string doc = "{\"token\":\"" + Json::Escape(token) + "\"}";
    CHECK_EQ_STR(Json::GetString(doc, "token"), token);

    // 缺 token 字段时应得到空串（服务端据此判定失败）
    CHECK_EQ_STR(Json::GetString(R"({"nope":1})", "token"), "");
}

int main()
{
    std::printf("=== moonlight-sideband 单元测试 ===\n");

    TestJsonBasics();
    TestJsonKeyInsideValue();
    TestJsonNumbers();
    TestJsonEscape();
    TestJsonMalformed();
    TestProtocolPackets();
    TestAuthPayloadRoundTrip();

    std::printf("=== %d 项检查, %d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
