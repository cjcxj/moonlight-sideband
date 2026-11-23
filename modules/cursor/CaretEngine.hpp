#pragma once

/**
 * CaretEngine —— 串行调度各 Caret Provider 的核心引擎（仅运行在 m_textCursorThread 上）
 *
 * 职责：
 * 1. 构建 CaretContextSnapshot
 * 2. 查询按 (hwnd, pid, focusClass) 索引的 LRU 亲和度路由表
 * 3. 首选直达 + 顺序回退探索（Win32 -> AppMsg -> MSAA -> UIA -> IMEAnchor -> ControlBox）
 * 4. 坐标有效性复核（CaretSanity）
 * 5. 显示器坐标映射、节流判定（CmdID=2 与 CmdID=102 分离发送）
 */

#include "CaretTypes.hpp"
#include "CaretLogic.hpp"
#include "CaretProviders.hpp"

namespace Caret {

class CaretEngine {
public:
    explicit CaretEngine(CaretOptions opts = {});
    ~CaretEngine();

    CaretEngine(const CaretEngine &) = delete;
    CaretEngine &operator=(const CaretEngine &) = delete;

    struct DetectOutput {
        CaretResult result;
        PixelRect monitorRect{};
        bool hasMonitor = false;
        CaretPublishTracker::PublishDecision publish{};
        std::string extJson;
    };

    // 执行一轮探测。必须在 m_textCursorThread 上调用。
    DetectOutput DetectAndEvaluate(const CaretContextSnapshot *eventHint,
                                   HWND eventHwnd,
                                   LONG eventIdObject,
                                   LONG eventIdChild,
                                   bool forcePublish);

    void ResetState();
    void ShutdownProviders();

    const CaretOptions &Options() const { return m_options; }

private:
    bool ValidateResult(const CaretContextSnapshot &ctx,
                        const CaretResult &res,
                        PixelRect &outMonitorRect,
                        bool &outHasMonitor) const;

    ProbeStatus RunSingleProvider(ProviderId id,
                                  const CaretContextSnapshot &ctx,
                                  CaretAffinityCache::Entry &entry,
                                  HWND eventHwnd,
                                  LONG eventIdObject,
                                  LONG eventIdChild,
                                  CaretResult &outResult,
                                  CaretResult *outControlBoxFallback);

    CaretOptions m_options;
    CaretAffinityCache m_affinity;
    CaretPublishTracker m_publishTracker;

    Win32CaretProvider m_win32;
    AppMsgCaretProvider m_appMsg;
    MSAACaretProvider m_msaa;
    UIACaretProvider m_uia;
    IMECaretProvider m_ime;
};

} // namespace Caret
