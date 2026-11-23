#pragma once

/**
 * Windows 平台文本插入符探测 Providers（全部仅在 Caret 工作线程上调用）
 *
 * 包含：
 * - BuildCaretContext:     通过 GetGUIThreadInfo(0) + 前台/焦点窗口构建上下文
 * - Win32CaretProvider:    Source=1，带闪烁宽限期与跨 DPI 物理坐标换算
 * - AppMsgCaretProvider:   Source=7(老客户端映射为1)，无指针跨进程消息覆盖 Edit 与 Scintilla
 * - MSAACaretProvider:     Source=2，OBJID_CARET 事件对象 + AccessibleObjectFromWindow
 * - UIACaretProvider:      Source=3，CUIAutomation8 超时 + CacheRequest + TextEditPattern +
 *                          TextPattern2 + 闸门保护的 TextPattern + 换行符修正 + 浏览器懒加载唤醒
 * - IMECaretProvider:      Source=5，IME 组合/候选窗口位置兜底
 * - ControlBoxProvider:    Source=6，仅输入控件外框兜底（非精确插入符）
 */

#include "CaretTypes.hpp"
#include "CaretLogic.hpp"

#include <windows.h>

struct IUIAutomation;
struct IUIAutomationCacheRequest;

namespace Caret {

enum class ProbeStatus {
    Hit,          // 拿到有效结果
    Miss,         // 目标正常但当前无此路径插入符
    HardFailure   // COM/IPC 失败或超时，计入熔断冷却
};

// 从线程本地 DPI 上下文安全把客户区矩形转到屏幕物理像素
bool ClientRectToScreenPhysical(HWND hwnd, const RECT &clientRc, PixelRect &outScreenRc);

// 构建当前探测上下文
CaretContextSnapshot BuildCaretContext(const CaretContextSnapshot *eventHint);

class Win32CaretProvider {
public:
    ProbeStatus Probe(const CaretContextSnapshot &ctx,
                      const CaretOptions &opts,
                      CaretResult &outResult);
    void Reset() { m_blinkTracker.Reset(); }

private:
    Win32BlinkTracker m_blinkTracker;
};

class AppMsgCaretProvider {
public:
    ProbeStatus Probe(const CaretContextSnapshot &ctx, CaretResult &outResult);
};

class MSAACaretProvider {
public:
    ProbeStatus Probe(const CaretContextSnapshot &ctx,
                      HWND eventHwnd,
                      LONG eventIdObject,
                      LONG eventIdChild,
                      CaretResult &outResult);
};

class UIACaretProvider {
public:
    UIACaretProvider() = default;
    ~UIACaretProvider();

    UIACaretProvider(const UIACaretProvider &) = delete;
    UIACaretProvider &operator=(const UIACaretProvider &) = delete;

    // 必须在工作线程（MTA）退出前调用，确保 COM 指针在 CoUninitialize 前释放
    void Shutdown();

    // 若目标进程处于高权限/管理员且本进程为普通权限，记录一次 UIPI 诊断提示
    void CheckAndLogElevatedTarget(uint32_t targetPid);

    ProbeStatus Probe(const CaretContextSnapshot &ctx,
                      CaretAffinityCache::Entry &affinityEntry,
                      CaretResult &outResult,
                      CaretResult *outControlBoxFallback);

    // 当 EVENT_OBJECT_IME_SHOW 未给出有效小矩形时，从 Win10/11 TextInputHost 查找候选窗边界
    bool FindImeCandidateRect(PixelRect &outRect);

private:
    bool EnsureInitialized();
    void MaybeWakeBrowserAccessibility(const CaretContextSnapshot &ctx,
                                       const std::wstring &uiaClass,
                                       CaretAffinityCache::Entry &affinityEntry);

    IUIAutomation *m_uia = nullptr;
    IUIAutomationCacheRequest *m_cacheReq = nullptr;
    bool m_comInited = false;
    bool m_broken = false;
    uint64_t m_nextRetryMs = 0;
};

class IMECaretProvider {
public:
    ProbeStatus Probe(const CaretContextSnapshot &ctx,
                      const CaretOptions &opts,
                      const PixelRect *overrideImeRect,
                      CaretResult &outResult);
};

} // namespace Caret
