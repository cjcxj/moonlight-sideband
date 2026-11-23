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
#include "cursor/CaretLogic.hpp"

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

    // found：区分"键不存在"与"值为 false"。AUDIO_MUTE_SET 靠它把
    // {"device_id":"x"} 判成 invalid_payload，而不是当成"取消静音"执行下去。
    bool found = true;
    CHECK(Json::GetBool(R"({"muted":false})", "muted", true, &found) == false && found);
    found = true;
    CHECK(Json::GetBool(R"({"device_id":"x"})", "muted", true, &found) == true && !found);
    found = true;
    CHECK(Json::GetBool(R"({"muted":"false"})", "muted", false, &found) == false && !found);

    // 值内部出现的键名不能被当成键（音频端点 ID 里就是大括号+点号这种形状）
    CHECK_EQ_STR(Json::GetString(R"({"msg":"device_id bad","device_id":"{0.0.0.00000000}.{ab}"})",
                                 "device_id"),
                 "{0.0.0.00000000}.{ab}");
    CHECK_EQ_INT(Json::GetInt(R"({"name":"volume 3","volume":70})", "volume"), 70);
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
    // 包尾两个保留字段：老语义保持 0/0（默认参数），不破坏老客户端
    CHECK_EQ_INT(ReadU32(txt, 16), 0);
    CHECK_EQ_INT(ReadU32(txt, 20), 0);

    // 新语义：保留字段填插入符高度 + 来源标记，包长与老格式一致
    auto txt2 = BuildTextCursorPacket(1234, 22, CARET_SOURCE_WIN32);
    CHECK_EQ_INT(txt2.size(), TEXT_CURSOR_PACKET_SIZE);
    CHECK_EQ_INT(ReadU32(txt2, 16), 22);
    CHECK_EQ_INT(ReadU32(txt2, 20), CARET_SOURCE_WIN32);
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

static void TestCaretSanityAndClassFilters()
{
    std::printf("Caret 假光标过滤与窗口类名分类\n");

    using namespace Caret;
    CHECK(IsBrowserOrWebViewClass(L"Chrome_WidgetWin_1"));
    CHECK(IsBrowserOrWebViewClass(L"Chrome_RenderWidgetHostHWND"));
    CHECK(IsBrowserOrWebViewClass(L"MozillaWindowClass"));
    CHECK(IsKnownSelfDrawnHostClass(L"Qt5152QWindowIcon"));
    CHECK(IsKnownSelfDrawnHostClass(L"UnityWndClass"));
    CHECK(IsScintillaClass(L"Scintilla"));
    CHECK(IsClassicEditClass(L"RICHEDIT50W"));
    CHECK(IsWindowsTerminalClass(L"CASCADIA_HOSTING_WINDOW_CLASS"));
    CHECK(!IsBrowserOrWebViewClass(L"Notepad"));

    const PixelRect client{0, 0, 1200, 800};

    // 1x1 占位哑光标（在 0,0 附近）必须被过滤
    CHECK(IsDummyWin32Caret(PixelRect{0, 0, 1, 1}, client, false));
    CHECK(IsDummyWin32Caret(PixelRect{2, 2, 3, 5}, client, false));

    // 自绘宿主里的 (0,0) 2x16 小光标也应判定为哑光标
    CHECK(IsDummyWin32Caret(PixelRect{0, 0, 2, 16}, client, true));

    // 正常文本插入符（比如 2x20，即使在 (0, 200) 或正常输入区）必须放行
    CHECK(!IsDummyWin32Caret(PixelRect{100, 200, 102, 220}, client, false));
    // 零宽高行矩形（如部分控件）也要放行
    CHECK(!IsDummyWin32Caret(PixelRect{50, 100, 50, 120}, client, false));
    // 超出窗口可视客户区纵向范围的异常坐标必须过滤
    CHECK(IsDummyWin32Caret(PixelRect{50, 900, 52, 920}, client, false));

    // 屏幕/窗口范围校验
    const PixelRect win{100, 100, 1100, 900};
    const PixelRect mon{0, 0, 2560, 1440};
    CHECK(IsCaretRectReasonable(PixelRect{200, 300, 202, 322}, win, mon, false));
    CHECK(!IsCaretRectReasonable(PixelRect{200, 1500, 202, 1522}, win, mon, false));
    CHECK(!IsCaretRectReasonable(PixelRect{200, 300, 202, 302}, win, mon, false)); // 太矮
}

static void TestWin32BlinkGrace()
{
    std::printf("Win32 闪烁暗灭期宽限状态机\n");

    using namespace Caret;
    Win32BlinkTracker tracker;
    const PixelRect client{0, 0, 800, 600};
    const PixelRect caretRc{120, 200, 122, 220};
    PixelRect out{};

    // 亮起期命中
    CHECK(tracker.Evaluate(true, 100, 101, 102, caretRc, client, false, 1000, 800, false, out));
    CHECK_EQ_INT(out.bottom, 220);

    // 暗灭期但在 800ms 宽限内，且三句柄一致 -> 保持有效
    CHECK(tracker.Evaluate(false, 100, 101, 102, caretRc, client, false, 1500, 800, false, out));
    CHECK_EQ_INT(out.bottom, 220);

    // 焦点窗口切换 -> 即使在宽限时间内也立刻失效
    CHECK(!tracker.Evaluate(false, 100, 999, 102, caretRc, client, false, 1600, 800, false, out));

    // 超出宽限时间 -> 失效
    CHECK(tracker.Evaluate(true, 100, 101, 102, caretRc, client, false, 2000, 800, false, out));
    CHECK(!tracker.Evaluate(false, 100, 101, 102, caretRc, client, false, 2900, 800, false, out));
}

static void TestNewlineAndAffinityCache()
{
    std::printf("行尾换行调整与亲和度/熔断缓存\n");

    using namespace Caret;
    PixelRect newLine{};
    // 前字符是 '\n' -> 光标应该落在下一行左缘
    CHECK(AdjustLineEndAfterNewline(PixelRect{300, 100, 310, 124}, 40, true, L'\n', newLine));
    CHECK_EQ_INT(newLine.top, 124);
    CHECK_EQ_INT(newLine.bottom, 148);
    CHECK_EQ_INT(newLine.left, 40);

    // 非换行符不应触发换行调整
    CHECK(!AdjustLineEndAfterNewline(PixelRect{300, 100, 310, 124}, 40, true, L'a', newLine));

    CaretAffinityCache cache;
    auto &e = cache.GetOrCreate(CaretAffinityCache::Key{1234, 5678, L"Chrome_WidgetWin_1"});
    CaretAffinityCache::RecordSuccess(e, ProviderId::UIA);
    CHECK(e.hasPreferred);
    CHECK(e.preferred == ProviderId::UIA);

    // 连续 3 次硬失败触发 2000ms 冷却并清除偏好
    CaretAffinityCache::RecordHardFailure(e, ProviderId::UIA, 1000, 3, 2000);
    CHECK(!CaretAffinityCache::IsInCooldown(e, ProviderId::UIA, 1000));
    CaretAffinityCache::RecordHardFailure(e, ProviderId::UIA, 1100, 3, 2000);
    CaretAffinityCache::RecordHardFailure(e, ProviderId::UIA, 1200, 3, 2000);
    CHECK(CaretAffinityCache::IsInCooldown(e, ProviderId::UIA, 2000));
    CHECK(!CaretAffinityCache::IsInCooldown(e, ProviderId::UIA, 3201));
    CHECK(!e.hasPreferred);
}

static void TestCaretPublishAndProtocol()
{
    std::printf("显示器 YPercent 换算、节流与扩展协议\n");

    using namespace Caret;
    using namespace SidebandProtocol;

    // 副屏（y: 1440..2880，高 1440），底边在 2160 -> 恰好 5000 (50.00%)
    const PixelRect mon2{0, 1440, 2560, 2880};
    CHECK_EQ_INT(ComputeMonitorPercent(2160, mon2.top, mon2.Height()), 5000);

    CaretOptions opts;
    opts.allowImeFallback = true;
    opts.allowControlBoxForLegacy = false;

    CaretPublishTracker pub;
    CaretResult r;
    r.valid = true;
    r.rect = PixelRect{500, 2136, 502, 2160};
    r.controlRect = PixelRect{400, 2120, 900, 2180};
    r.hasControlRect = true;
    r.source = CARET_SOURCE_UIA;
    r.confidence = CaretConfidence::Exact;

    auto d1 = pub.Evaluate(r, mon2, false, opts);
    CHECK(d1.sendLegacy);
    CHECK_EQ_INT(d1.legacy.yPercent, 5000);
    CHECK_EQ_INT(d1.legacy.height, 24);
    CHECK_EQ_INT(d1.legacy.source, CARET_SOURCE_UIA);
    CHECK(d1.sendExt);
    CHECK_EQ_INT(d1.extY, 5000);

    // 1px 微小抖动（在 0.15% 死区内）不应触发广播
    r.rect.bottom = 2161;
    auto d2 = pub.Evaluate(r, mon2, false, opts);
    CHECK(!d2.sendLegacy);
    CHECK(!d2.sendExt);

    // 仅有 ControlBox 时：老协议被拦截成 -1（退出），扩展协议仍可下发 Estimated
    CaretResult box;
    box.valid = true;
    box.rect = PixelRect{400, 2120, 900, 2180};
    box.controlRect = box.rect;
    box.hasControlRect = true;
    box.source = CARET_SOURCE_CONTROL_BOX;
    box.confidence = CaretConfidence::Estimated;

    auto d3 = pub.Evaluate(box, mon2, false, opts);
    CHECK(d3.sendLegacy);
    CHECK_EQ_INT(d3.legacy.yPercent, -1);
    CHECK(d3.sendExt);
    CHECK_EQ_INT(d3.extY, ComputeMonitorPercent(2180, mon2.top, mon2.Height()));

    std::string extJson = BuildTextCursorExtJson(box, d3.extY, d3.extX, d3.extCtrlBottom);
    CHECK_EQ_INT(Json::GetInt(extJson, "src"), CARET_SOURCE_CONTROL_BOX);
    CHECK_EQ_INT(Json::GetInt(extJson, "conf"), static_cast<int>(CaretConfidence::Estimated));
    CHECK(Json::GetBool(extJson, "comp", true) == false);

    // HELLO 能力协商字段解析
    CHECK(Json::GetBool(R"({"proto_ver":1,"caret_ext":true})", "caret_ext", false) == true);
    CHECK(Json::GetBool(R"({"proto_ver":1})", "caret_ext", false) == false);
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
    TestCaretSanityAndClassFilters();
    TestWin32BlinkGrace();
    TestNewlineAndAffinityCache();
    TestCaretPublishAndProtocol();

    std::printf("=== %d 项检查, %d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
