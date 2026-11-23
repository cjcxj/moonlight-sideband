#include "CaretEngine.hpp"
#include "Logger.hpp"

#include <algorithm>
#include <array>

namespace Caret {

CaretEngine::CaretEngine(CaretOptions opts)
    : m_options(opts)
{
}

CaretEngine::~CaretEngine()
{
    ShutdownProviders();
}

void CaretEngine::ShutdownProviders()
{
    m_uia.Shutdown();
}

void CaretEngine::ResetState()
{
    m_win32.Reset();
    m_affinity.Clear();
    m_publishTracker.Reset();
}

bool CaretEngine::ValidateResult(const CaretContextSnapshot &ctx,
                                 const CaretResult &res,
                                 PixelRect &outMonitorRect,
                                 bool &outHasMonitor) const
{
    outHasMonitor = false;
    outMonitorRect = PixelRect{};

    if (!res.valid)
        return false;

    POINT pt{res.rect.CenterX(), res.rect.bottom};
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (hMon && GetMonitorInfoW(hMon, &mi))
    {
        outHasMonitor = true;
        outMonitorRect = PixelRect{
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right, mi.rcMonitor.bottom
        };
    }

    const PixelRect winBound = SelectValidationWindowBound(
        ctx.focusWindowRect, ctx.foregroundWindowRect, res.rect);
    const bool isControlBox = (res.source == SidebandProtocol::CARET_SOURCE_CONTROL_BOX);
    const bool isImeAnchor = (res.source == SidebandProtocol::CARET_SOURCE_IME_ANCHOR);

    return IsCaretRectReasonable(res.rect, winBound, outMonitorRect, isControlBox, isImeAnchor);
}

ProbeStatus CaretEngine::RunSingleProvider(ProviderId id,
                                           const CaretContextSnapshot &ctx,
                                           CaretAffinityCache::Entry &entry,
                                           HWND eventHwnd,
                                           LONG eventIdObject,
                                           LONG eventIdChild,
                                           CaretResult &outResult,
                                           CaretResult *outControlBoxFallback)
{
    switch (id)
    {
    case ProviderId::Win32:
        return m_win32.Probe(ctx, m_options, outResult);

    case ProviderId::AppMsg:
        return m_appMsg.Probe(ctx, outResult);

    case ProviderId::MSAA:
        return m_msaa.Probe(ctx, eventHwnd, eventIdObject, eventIdChild, outResult);

    case ProviderId::UIA:
        return m_uia.Probe(ctx, entry, outResult, outControlBoxFallback);

    case ProviderId::IME:
    {
        ProbeStatus st = m_ime.Probe(ctx, m_options, nullptr, false, outResult);
        if (st == ProbeStatus::Miss && m_options.allowImeFallback)
        {
            const bool recentIme =
                ctx.imeVisible &&
                ctx.lastImeEventMs > 0 &&
                ctx.nowMs >= ctx.lastImeEventMs &&
                (ctx.nowMs - ctx.lastImeEventMs) <= 8000ULL;
            const bool recentKb =
                ctx.lastKeyboardInputMs > 0 &&
                ctx.nowMs >= ctx.lastKeyboardInputMs &&
                (ctx.nowMs - ctx.lastKeyboardInputMs) <= 5000ULL;

            bool imeWinValid = false;
            if (!ctx.imeWindowRect.IsEmpty())
            {
                int32_t monW = 1920, monH = 1080;
                POINT pt{ctx.imeWindowRect.CenterX(), ctx.imeWindowRect.CenterY()};
                HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (hMon && GetMonitorInfoW(hMon, &mi))
                {
                    monW = std::max<int32_t>(1, mi.rcMonitor.right - mi.rcMonitor.left);
                    monH = std::max<int32_t>(1, mi.rcMonitor.bottom - mi.rcMonitor.top);
                }
                imeWinValid = IsCandidateWindowRectValid(ctx.imeWindowRect, monW, monH);
            }

            const bool needHostLookup = !recentIme || !imeWinValid;
            const uint64_t cooldownMs = (recentIme || recentKb) ? 100ULL : 350ULL;

            if (needHostLookup &&
                (m_lastTextHostProbeMs == 0 || ctx.nowMs >= m_lastTextHostProbeMs + cooldownMs))
            {
                m_lastTextHostProbeMs = ctx.nowMs;
                PixelRect hostImeRc{};
                if (m_uia.FindImeCandidateRect(hostImeRc))
                    st = m_ime.Probe(ctx, m_options, &hostImeRc, true, outResult);
            }
        }
        return st;
    }

    default:
        return ProbeStatus::Miss;
    }
}

CaretEngine::DetectOutput CaretEngine::DetectAndEvaluate(const CaretContextSnapshot *eventHint,
                                                         HWND eventHwnd,
                                                         LONG eventIdObject,
                                                         LONG eventIdChild,
                                                         bool forcePublish)
{
    DetectOutput out;

    CaretContextSnapshot ctx = BuildCaretContext(eventHint);
    CaretAffinityCache::Key key{
        ctx.focusedWindow ? ctx.focusedWindow : ctx.foregroundWindow,
        ctx.processId,
        ctx.focusClassName
    };
    CaretAffinityCache::Entry &entry = m_affinity.GetOrCreate(key);

    // 构造探测序列：若存在有效亲和度 Provider，放在首位直达；其余按标准回退链顺序补全
    std::array<ProviderId, 5> order = {
        ProviderId::Win32,
        ProviderId::AppMsg,
        ProviderId::MSAA,
        ProviderId::UIA,
        ProviderId::IME
    };

    if (entry.hasPreferred &&
        entry.preferred != ProviderId::Win32 &&
        entry.preferred != ProviderId::ControlBox &&
        !CaretAffinityCache::IsInCooldown(entry, entry.preferred, ctx.nowMs))
    {
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (order[i] == entry.preferred)
            {
                for (size_t j = i; j > 0; --j)
                    order[j] = order[j - 1];
                order[0] = entry.preferred;
                break;
            }
        }
    }

    CaretResult bestResult;
    CaretResult controlBoxFallback;
    PixelRect bestMonitor{};
    bool bestHasMonitor = false;

    for (ProviderId pid : order)
    {
        if (CaretAffinityCache::IsInCooldown(entry, pid, ctx.nowMs))
            continue;

        CaretResult candidate;
        CaretResult *boxPtr = (!controlBoxFallback.valid && pid == ProviderId::UIA)
                                  ? &controlBoxFallback
                                  : nullptr;

        ProbeStatus st = RunSingleProvider(pid, ctx, entry,
                                           eventHwnd, eventIdObject, eventIdChild,
                                           candidate, boxPtr);

        if (st == ProbeStatus::HardFailure)
        {
            CaretAffinityCache::RecordHardFailure(entry, pid, ctx.nowMs,
                                                  m_options.failureThreshold,
                                                  m_options.providerCooldownMs);
            continue;
        }

        if (st == ProbeStatus::Hit)
        {
            PixelRect monRc{};
            bool hasMon = false;
            if (ValidateResult(ctx, candidate, monRc, hasMon) && hasMon)
            {
                CaretAffinityCache::RecordSuccess(entry, pid);
                bestResult = std::move(candidate);
                bestMonitor = monRc;
                bestHasMonitor = hasMon;
                break;
            }
            else if (pid == ProviderId::Win32)
            {
                // 单帧坐标不合理（滚动/动画/跨 DPI）：短暂冷却，下一轮仍给机会
                CaretAffinityCache::RecordSanityMiss(entry, pid, ctx.nowMs,
                                                     m_options.providerCooldownMs);
            }
        }
    }

    // 所有精确路径与 IME 锚点均失败时，检查 UIA 顺带取回的 ControlBox 兜底
    if (!bestResult.valid && controlBoxFallback.valid)
    {
        PixelRect monRc{};
        bool hasMon = false;
        if (ValidateResult(ctx, controlBoxFallback, monRc, hasMon) && hasMon)
        {
            bestResult = std::move(controlBoxFallback);
            bestMonitor = monRc;
            bestHasMonitor = hasMon;
            if (!m_options.allowControlBoxForLegacy)
            {
                static bool s_loggedBlockedBox = false;
                if (!s_loggedBlockedBox)
                {
                    s_loggedBlockedBox = true;
                    Logger::Get().Warning("CaretEngine: 已命中输入框矩形(ControlBox)，但 moonlight_sideband.ini 中 caret_allow_control_box=false 拦截了老协议推送");
                }
            }
        }
        else
        {
            Logger::Get().Debug("[文本光标] ControlBox 校验未通过 rect=(",
                                controlBoxFallback.rect.left, ",", controlBoxFallback.rect.top, ",",
                                controlBoxFallback.rect.right, ",", controlBoxFallback.rect.bottom,
                                ") hasMon=", hasMon ? 1 : 0);
        }
    }

    if (!bestResult.valid && ctx.processId != 0)
        m_uia.CheckAndLogElevatedTarget(ctx.processId);

    out.result = bestResult;
    out.monitorRect = bestMonitor;
    out.hasMonitor = bestHasMonitor;
    out.publish = m_publishTracker.Evaluate(bestResult, bestMonitor, forcePublish, m_options);
    if (out.publish.sendExt)
    {
        out.extJson = BuildTextCursorExtJson(bestResult,
                                             out.publish.extY,
                                             out.publish.extX,
                                             out.publish.extCtrlBottom);
    }

    return out;
}

} // namespace Caret
