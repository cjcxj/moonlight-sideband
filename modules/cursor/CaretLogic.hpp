#pragma once

/**
 * 文本插入符纯逻辑引擎（无 Windows API 依赖，可直接走 sideband_tests 单测）
 *
 * 包含：
 * 1. 窗口类名分类（自绘宿主 / 浏览器 / Scintilla / 经典 Edit / Windows Terminal）
 * 2. 假光标/哑光标过滤与坐标合理性校验（CaretSanity）
 * 3. Win32 光标闪烁暗灭期宽限状态机（Win32BlinkTracker）
 * 4. 行尾换行符（\r / \n）后光标落点修正
 * 5. 按 (hwnd, pid, focusClass) 索引的 LRU 亲和度路由与 Provider 熔断冷却表
 * 6. 显示器相对 YPercent 计算、死区节流与新旧协议映射
 */

#include "CaretTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Caret {

// ============================================================
// 1. 窗口类名分类辅助
// ============================================================

inline bool StartsWithCaseInsensitive(std::wstring_view s, std::wstring_view prefix)
{
    if (s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        wchar_t a = s[i];
        wchar_t b = prefix[i];
        if (a >= L'A' && a <= L'Z') a += (L'a' - L'A');
        if (b >= L'A' && b <= L'Z') b += (L'a' - L'A');
        if (a != b)
            return false;
    }
    return true;
}

inline bool ContainsCaseInsensitive(std::wstring_view s, std::wstring_view needle)
{
    if (needle.empty())
        return true;
    if (s.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i)
    {
        if (StartsWithCaseInsensitive(s.substr(i), needle))
            return true;
    }
    return false;
}

inline bool IsWindowsTerminalClass(std::wstring_view cls)
{
    return cls == L"CASCADIA_HOSTING_WINDOW_CLASS";
}

inline bool IsScintillaClass(std::wstring_view cls)
{
    return StartsWithCaseInsensitive(cls, L"Scintilla");
}

inline bool IsClassicEditClass(std::wstring_view cls)
{
    return cls == L"Edit" ||
           StartsWithCaseInsensitive(cls, L"RichEdit") ||
           StartsWithCaseInsensitive(cls, L"RICHEDIT") ||
           cls == L"ComboLBox";
}

inline bool IsBrowserOrWebViewClass(std::wstring_view cls)
{
    return StartsWithCaseInsensitive(cls, L"Chrome_WidgetWin") ||
           cls == L"Chrome_RenderWidgetHostHWND" ||
           cls == L"MozillaWindowClass" ||
           cls == L"CefBrowserWindow" ||
           StartsWithCaseInsensitive(cls, L"WebView") ||
           StartsWithCaseInsensitive(cls, L"Intermediate D3D Window");
}

inline bool IsKnownSelfDrawnHostClass(std::wstring_view cls)
{
    if (IsBrowserOrWebViewClass(cls) || IsWindowsTerminalClass(cls))
        return true;
    return StartsWithCaseInsensitive(cls, L"Qt") ||
           ContainsCaseInsensitive(cls, L"QWindow") ||
           cls == L"SDL_app" ||
           StartsWithCaseInsensitive(cls, L"GLFW") ||
           cls == L"UnityWndClass" ||
           cls == L"UnrealWindow" ||
           StartsWithCaseInsensitive(cls, L"WeChatMainWnd") ||
           StartsWithCaseInsensitive(cls, L"ChatWnd") ||
           StartsWithCaseInsensitive(cls, L"SunAwt");
}

// ============================================================
// 2. CaretSanity：哑光标过滤与边界校验
// ============================================================

inline bool IsDummyWin32Caret(const PixelRect &caretClientRect,
                              const PixelRect &clientRect,
                              bool isKnownSelfDrawnHost)
{
    const int32_t w = caretClientRect.Width();
    const int32_t h = caretClientRect.Height();

    // 高度非法或极大（整块画布而不是光标）
    if (h <= 0 || h > 250)
        return true;

    // 经典的 IME 占位哑光标：CreateCaret(hwnd, NULL, 1, 1) 放在客户区 (0,0) 附近
    if (w <= 2 && h < 6)
    {
        if (caretClientRect.left <= 4 && caretClientRect.top <= 4)
            return true;
        // 即便不在 (0,0)，1x1 或高度 <4 的 Win32 插入符几乎必为占位符
        if (w <= 1 && h <= 2)
            return true;
    }

    // 浏览器 / Qt / 游戏 / 微信宿主类窗口中出现的超小或固定左上角小矩形
    if (isKnownSelfDrawnHost)
    {
        if (h < 8 && w <= 2)
            return true;
        if (caretClientRect.left == 0 && caretClientRect.top == 0 && w <= 2 && h <= 16)
            return true;
    }

    // 若客户区尺寸有效且插入符完全落在可视客户区之外（纵向）
    if (!clientRect.IsEmpty())
    {
        if (caretClientRect.bottom <= clientRect.top - 8 ||
            caretClientRect.top >= clientRect.bottom + 8)
            return true;
    }

    return false;
}

inline bool IsCaretRectReasonable(const PixelRect &screenRect,
                                  const PixelRect &windowRect,
                                  const PixelRect &monitorRect,
                                  bool isControlBox)
{
    const int32_t h = screenRect.Height();
    if (h < (isControlBox ? 10 : 5) || h > 280)
        return false;

    if (screenRect.right < screenRect.left)
        return false;

    // 若显示器矩形有效，插入符底边不应严重脱离显示器范围
    if (!monitorRect.IsEmpty())
    {
        if (screenRect.bottom < monitorRect.top - 32 ||
            screenRect.top > monitorRect.bottom + 32 ||
            screenRect.right < monitorRect.left - 64 ||
            screenRect.left > monitorRect.right + 64)
            return false;
    }

    // 若所属窗口矩形有效，插入符不应严重飘到窗口外面（允许少量边框/滚动容差）
    if (!windowRect.IsEmpty())
    {
        constexpr int32_t kTol = 48;
        if (screenRect.bottom < windowRect.top - kTol ||
            screenRect.top > windowRect.bottom + kTol ||
            screenRect.right < windowRect.left - kTol ||
            screenRect.left > windowRect.right + kTol)
            return false;
    }

    return true;
}

// ============================================================
// 3. Win32BlinkTracker：解决 GUI_CARETBLINKING 闪烁暗灭期丢帧
// ============================================================

struct Win32BlinkTracker {
    uint64_t lastBlinkSeenMs = 0;
    uint64_t hwndCaret = 0;
    uint64_t hwndFocus = 0;
    uint64_t hwndForeground = 0;
    PixelRect lastClientRect{};
    bool hasValid = false;

    void Reset()
    {
        hasValid = false;
        hwndCaret = 0;
        hwndFocus = 0;
        hwndForeground = 0;
        lastBlinkSeenMs = 0;
        lastClientRect = PixelRect{};
    }

    // 输入本轮 GetGUIThreadInfo 的原始字段，返回是否应采信以及采信的客户区矩形
    bool Evaluate(bool isBlinking,
                  uint64_t curHwndCaret,
                  uint64_t curHwndFocus,
                  uint64_t curHwndForeground,
                  const PixelRect &curClientRect,
                  const PixelRect &focusClientRect,
                  bool isKnownSelfDrawnHost,
                  uint64_t nowMs,
                  int32_t graceMs,
                  bool infiniteBlink,
                  PixelRect &outClientRect)
    {
        if (curHwndCaret == 0)
        {
            Reset();
            return false;
        }

        // 先对当前矩形做哑光标过滤（只要矩形本身非空）
        const bool curDummy = IsDummyWin32Caret(curClientRect, focusClientRect, isKnownSelfDrawnHost);

        if (isBlinking || infiniteBlink)
        {
            if (curDummy)
            {
                // 闪烁中的哑光标直接拒绝并清空旧状态
                Reset();
                return false;
            }
            hasValid = true;
            lastBlinkSeenMs = nowMs;
            hwndCaret = curHwndCaret;
            hwndFocus = curHwndFocus;
            hwndForeground = curHwndForeground;
            lastClientRect = curClientRect;
            outClientRect = curClientRect;
            return true;
        }

        // 暗灭期：必须确认 caret/focus/foreground 三句柄仍与上次亮起一致，且在宽限窗口内
        if (hasValid &&
            curHwndCaret == hwndCaret &&
            curHwndFocus == hwndFocus &&
            curHwndForeground == hwndForeground &&
            nowMs >= lastBlinkSeenMs &&
            (nowMs - lastBlinkSeenMs) <= static_cast<uint64_t>(graceMs))
        {
            // 若系统在本轮给出了同样合理的矩形，优先用最新矩形；否则沿用上次有效矩形
            if (!curDummy)
                lastClientRect = curClientRect;
            outClientRect = lastClientRect;
            return true;
        }

        Reset();
        return false;
    }
};

// ============================================================
// 4. 行尾换行符修正（向前回退借 1 个字符时，若前字符是 \r / \n）
// ============================================================

inline bool AdjustLineEndAfterNewline(const PixelRect &prevCharRect,
                                      int32_t controlLeft,
                                      bool hasControlLeft,
                                      wchar_t prevChar,
                                      PixelRect &outNewLineRect)
{
    if (prevChar != L'\n' && prevChar != L'\r')
        return false;

    int32_t lineH = prevCharRect.Height();
    if (lineH <= 0)
        lineH = 20;

    outNewLineRect.top = prevCharRect.bottom;
    outNewLineRect.bottom = outNewLineRect.top + lineH;
    outNewLineRect.left = hasControlLeft ? controlLeft : prevCharRect.left;
    outNewLineRect.right = outNewLineRect.left;
    return true;
}

// ============================================================
// 5. CaretAffinityCache：窗口亲和度路由 + Provider 连续失败熔断冷却
// ============================================================

class CaretAffinityCache {
public:
    static constexpr size_t kCapacity = 32;
    static constexpr size_t kProviderCount = static_cast<size_t>(ProviderId::Count);

    struct Key {
        uint64_t hwnd = 0;
        uint32_t processId = 0;
        std::wstring focusClass;

        bool operator==(const Key &o) const
        {
            return hwnd == o.hwnd && processId == o.processId && focusClass == o.focusClass;
        }
    };

    struct Entry {
        Key key;
        uint64_t lastUsedSeq = 0;
        bool hasPreferred = false;
        ProviderId preferred = ProviderId::Win32;
        bool dummyWin32 = false;
        uint64_t lastBrowserWakeMs = 0;
        std::array<int32_t, kProviderCount> consecutiveFails{};
        std::array<uint64_t, kProviderCount> cooldownUntilMs{};
    };

    Entry &GetOrCreate(const Key &key)
    {
        ++m_seq;
        for (auto &e : m_entries)
        {
            if (e.key == key)
            {
                e.lastUsedSeq = m_seq;
                return e;
            }
        }
        if (m_entries.size() < kCapacity)
        {
            Entry fresh;
            fresh.key = key;
            fresh.lastUsedSeq = m_seq;
            m_entries.push_back(std::move(fresh));
            return m_entries.back();
        }
        auto oldest = std::min_element(
            m_entries.begin(), m_entries.end(),
            [](const Entry &a, const Entry &b) { return a.lastUsedSeq < b.lastUsedSeq; });
        *oldest = Entry{};
        oldest->key = key;
        oldest->lastUsedSeq = m_seq;
        return *oldest;
    }

    const Entry *Find(const Key &key) const
    {
        for (const auto &e : m_entries)
        {
            if (e.key == key)
                return &e;
        }
        return nullptr;
    }

    static bool IsInCooldown(const Entry &e, ProviderId p, uint64_t nowMs)
    {
        const size_t idx = static_cast<size_t>(p);
        if (idx >= kProviderCount)
            return false;
        return e.cooldownUntilMs[idx] > nowMs;
    }

    static void RecordSuccess(Entry &e, ProviderId p)
    {
        const size_t idx = static_cast<size_t>(p);
        if (idx >= kProviderCount)
            return;
        e.consecutiveFails[idx] = 0;
        e.cooldownUntilMs[idx] = 0;
        if (p != ProviderId::IME && p != ProviderId::ControlBox)
        {
            e.hasPreferred = true;
            e.preferred = p;
        }
    }

    static void RecordHardFailure(Entry &e, ProviderId p, uint64_t nowMs,
                                  int32_t threshold, int32_t cooldownMs)
    {
        const size_t idx = static_cast<size_t>(p);
        if (idx >= kProviderCount)
            return;
        if (++e.consecutiveFails[idx] >= threshold)
        {
            e.cooldownUntilMs[idx] = nowMs + static_cast<uint64_t>(cooldownMs);
            e.consecutiveFails[idx] = 0;
            if (e.hasPreferred && e.preferred == p)
                e.hasPreferred = false;
        }
    }

    void Clear()
    {
        m_entries.clear();
        m_seq = 0;
    }

private:
    std::vector<Entry> m_entries;
    uint64_t m_seq = 0;
};

// ============================================================
// 6. 显示器比例换算、老协议映射、节流状态机与扩展 JSON 构造
// ============================================================

inline int32_t ComputeMonitorPercent(int32_t pos, int32_t monStart, int32_t monSpan)
{
    if (monSpan <= 0)
        return -1;
    double pct = (static_cast<double>(pos - monStart) * 10000.0) / static_cast<double>(monSpan);
    int32_t rounded = static_cast<int32_t>(std::llround(pct));
    return std::clamp(rounded, 0, 10000);
}

struct LegacyCaretPacketData {
    int32_t yPercent = -1;
    int32_t height = 0;
    int32_t source = SidebandProtocol::CARET_SOURCE_NONE;
};

// 将 CaretResult + 显示器范围映射为向后兼容的 CmdID=2 字段
inline LegacyCaretPacketData MapToLegacyPacket(const CaretResult &res,
                                               int32_t yPercent,
                                               const CaretOptions &opts)
{
    LegacyCaretPacketData out;
    if (!res.valid || yPercent < 0)
        return out;

    if (res.source == SidebandProtocol::CARET_SOURCE_IME_ANCHOR && !opts.allowImeFallback)
        return out;
    if (res.source == SidebandProtocol::CARET_SOURCE_CONTROL_BOX && !opts.allowControlBoxForLegacy)
        return out;

    out.yPercent = yPercent;
    out.height = std::max<int32_t>(0, res.rect.Height());

    switch (res.source)
    {
    case SidebandProtocol::CARET_SOURCE_WIN32:
    case SidebandProtocol::CARET_SOURCE_APP_MSG:
        out.source = SidebandProtocol::CARET_SOURCE_WIN32;
        break;
    case SidebandProtocol::CARET_SOURCE_MSAA:
        out.source = SidebandProtocol::CARET_SOURCE_MSAA;
        break;
    case SidebandProtocol::CARET_SOURCE_UIA:
    case SidebandProtocol::CARET_SOURCE_IA2:
        out.source = SidebandProtocol::CARET_SOURCE_UIA;
        break;
    case SidebandProtocol::CARET_SOURCE_IME_ANCHOR:
    case SidebandProtocol::CARET_SOURCE_CONTROL_BOX:
        out.source = res.source;
        break;
    default:
        out.source = SidebandProtocol::CARET_SOURCE_NONE;
        out.yPercent = -1;
        out.height = 0;
        break;
    }
    return out;
}

inline std::string BuildTextCursorExtJson(const CaretResult &res,
                                          int32_t yPercent,
                                          int32_t xPercent,
                                          int32_t ctrlBottomPercent)
{
    if (!res.valid || yPercent < 0)
    {
        return R"({"y":-1,"x":-1,"w":0,"h":0,"src":0,"conf":0,"comp":false,"pwd":false,"ctrl_bottom":-1})";
    }

    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "{\"y\":%d,\"x\":%d,\"w\":%d,\"h\":%d,\"src\":%d,\"conf\":%d,\"comp\":%s,\"pwd\":%s,\"ctrl_bottom\":%d}",
        yPercent,
        xPercent,
        std::max<int32_t>(0, res.rect.Width()),
        std::max<int32_t>(0, res.rect.Height()),
        res.source,
        static_cast<int32_t>(res.confidence),
        res.isComposition ? "true" : "false",
        res.isPassword ? "true" : "false",
        ctrlBottomPercent);
    return std::string(buf);
}

class CaretPublishTracker {
public:
    struct PublishDecision {
        bool sendLegacy = false;
        LegacyCaretPacketData legacy{};
        bool sendExt = false;
        int32_t extY = -1;
        int32_t extX = -1;
        int32_t extCtrlBottom = -1;
    };

    PublishDecision Evaluate(const CaretResult &res,
                             const PixelRect &monitorRect,
                             bool forceUpdate,
                             const CaretOptions &opts)
    {
        PublishDecision d;

        int32_t yPct = -1;
        int32_t xPct = -1;
        int32_t ctrlBottomPct = -1;

        if (res.valid && !monitorRect.IsEmpty())
        {
            yPct = ComputeMonitorPercent(res.rect.bottom, monitorRect.top, monitorRect.Height());
            xPct = ComputeMonitorPercent(res.rect.CenterX(), monitorRect.left, monitorRect.Width());
            if (res.hasControlRect && !res.controlRect.IsEmpty())
                ctrlBottomPct = ComputeMonitorPercent(res.controlRect.bottom, monitorRect.top, monitorRect.Height());
        }

        // 1. 老协议（CmdID=2）变更判断
        LegacyCaretPacketData legacy = MapToLegacyPacket(res, yPct, opts);
        if (ShouldEmit(legacy.yPercent, legacy.height, legacy.source,
                       m_lastLegacyY, m_lastLegacyH, m_lastLegacySrc,
                       forceUpdate, opts))
        {
            d.sendLegacy = true;
            d.legacy = legacy;
            m_lastLegacyY = legacy.yPercent;
            m_lastLegacyH = legacy.height;
            m_lastLegacySrc = legacy.source;
        }

        // 2. 扩展协议（CmdID=102）变更判断
        const int32_t extY = (res.valid && yPct >= 0) ? yPct : -1;
        const int32_t extH = (extY >= 0) ? std::max<int32_t>(0, res.rect.Height()) : 0;
        const int32_t extSrc = (extY >= 0) ? res.source : SidebandProtocol::CARET_SOURCE_NONE;
        const int32_t extConf = (extY >= 0) ? static_cast<int32_t>(res.confidence) : 0;

        if (ShouldEmit(extY, extH, extSrc,
                       m_lastExtY, m_lastExtH, m_lastExtSrc,
                       forceUpdate, opts) ||
            extConf != m_lastExtConf ||
            res.isComposition != m_lastExtComp)
        {
            d.sendExt = true;
            d.extY = extY;
            d.extX = (extY >= 0) ? xPct : -1;
            d.extCtrlBottom = (extY >= 0) ? ctrlBottomPct : -1;
            m_lastExtY = extY;
            m_lastExtH = extH;
            m_lastExtSrc = extSrc;
            m_lastExtConf = extConf;
            m_lastExtComp = res.isComposition;
        }

        return d;
    }

    void Reset()
    {
        m_lastLegacyY = -2;
        m_lastLegacyH = 0;
        m_lastLegacySrc = 0;
        m_lastExtY = -2;
        m_lastExtH = 0;
        m_lastExtSrc = 0;
        m_lastExtConf = 0;
        m_lastExtComp = false;
    }

private:
    static bool ShouldEmit(int32_t curY, int32_t curH, int32_t curSrc,
                           int32_t prevY, int32_t prevH, int32_t prevSrc,
                           bool forceUpdate, const CaretOptions &opts)
    {
        if (forceUpdate)
        {
            // 仍避免在连续无插入符时无谓重复发送 -1
            if (curY == -1 && prevY == -1)
                return false;
            return true;
        }
        if (prevY == -2)
            return true;
        if ((curY == -1) != (prevY == -1))
            return true;
        if (curY == -1)
            return false;
        if (curSrc != prevSrc)
            return true;
        if (std::abs(curY - prevY) >= opts.yPercentDeadzone)
            return true;
        if (std::abs(curH - prevH) >= opts.heightDeadzonePx)
            return true;
        return false;
    }

    int32_t m_lastLegacyY = -2;
    int32_t m_lastLegacyH = 0;
    int32_t m_lastLegacySrc = 0;

    int32_t m_lastExtY = -2;
    int32_t m_lastExtH = 0;
    int32_t m_lastExtSrc = 0;
    int32_t m_lastExtConf = 0;
    bool m_lastExtComp = false;
};

} // namespace Caret
