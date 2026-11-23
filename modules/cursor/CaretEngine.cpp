#include "CaretEngine.hpp"

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

    const PixelRect &winBound = !ctx.focusWindowRect.IsEmpty()
                                    ? ctx.focusWindowRect
                                    : ctx.foregroundWindowRect;
    const bool isControlBox = (res.source == SidebandProtocol::CARET_SOURCE_CONTROL_BOX);

    return IsCaretRectReasonable(res.rect, winBound, outMonitorRect, isControlBox);
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
        return m_ime.Probe(ctx, m_options, outResult);

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
        }
    }

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
