#include "CaretProviders.hpp"
#include "Logger.hpp"

#include <ole2.h>
#include <oleacc.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace Caret {
namespace {

// Scintilla 无指针跨进程消息常量
constexpr UINT kSciGetCurrentPos = 2008;
constexpr UINT kSciPointXFromPosition = 2164;
constexpr UINT kSciPointYFromPosition = 2165;
constexpr UINT kSciTextHeight = 2279;

// 经典 Edit 无指针跨进程消息（显式写出避免宏差异）
#ifndef EM_GETSEL
#define EM_GETSEL 0x00B0
#endif
#ifndef EM_POSFROMCHAR
#define EM_POSFROMCHAR 0x00D6
#endif

// 动态加载 Win10 DPI 转换函数，避免旧版系统/SDK 差异
struct DpiHelpers {
    using GetWindowDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI *)(HWND);
    using GetAwarenessFromDpiAwarenessContextFn = DPI_AWARENESS(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using LogicalToPhysicalForPMFn = BOOL(WINAPI *)(HWND, LPPOINT);

    GetWindowDpiAwarenessContextFn pGetWindowCtx = nullptr;
    GetAwarenessFromDpiAwarenessContextFn pGetAwareness = nullptr;
    SetThreadDpiAwarenessContextFn pSetThreadCtx = nullptr;
    LogicalToPhysicalForPMFn pLogicalToPhysical = nullptr;

    DpiHelpers()
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
        {
            pGetWindowCtx = reinterpret_cast<GetWindowDpiAwarenessContextFn>(
                GetProcAddress(u32, "GetWindowDpiAwarenessContext"));
            pGetAwareness = reinterpret_cast<GetAwarenessFromDpiAwarenessContextFn>(
                GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext"));
            pSetThreadCtx = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
                GetProcAddress(u32, "SetThreadDpiAwarenessContext"));
            pLogicalToPhysical = reinterpret_cast<LogicalToPhysicalForPMFn>(
                GetProcAddress(u32, "LogicalToPhysicalPointForPerMonitorDPI"));
        }
    }

    static const DpiHelpers &Get()
    {
        static DpiHelpers inst;
        return inst;
    }
};

PixelRect ToPixelRect(const RECT &r)
{
    return PixelRect{r.left, r.top, r.right, r.bottom};
}

std::wstring SafeGetClassName(HWND hwnd)
{
    if (!hwnd)
        return {};
    wchar_t buf[128] = {};
    int n = GetClassNameW(hwnd, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n <= 0)
        return {};
    return std::wstring(buf, static_cast<size_t>(n));
}

bool QueryTextPattern2(IUIAutomationElement *el, IUIAutomationTextPattern2 **out)
{
    *out = nullptr;
    IUnknown *pUnk = nullptr;
    HRESULT hr = el->GetCurrentPattern(UIA_TextPattern2Id, &pUnk);
    if (FAILED(hr) || !pUnk)
        return false;
    hr = pUnk->QueryInterface(IID_PPV_ARGS(out));
    pUnk->Release();
    return SUCCEEDED(hr) && *out;
}

bool QueryTextPattern(IUIAutomationElement *el, IUIAutomationTextPattern **out)
{
    *out = nullptr;
    IUnknown *pUnk = nullptr;
    HRESULT hr = el->GetCurrentPattern(UIA_TextPatternId, &pUnk);
    if (FAILED(hr) || !pUnk)
        return false;
    hr = pUnk->QueryInterface(IID_PPV_ARGS(out));
    pUnk->Release();
    return SUCCEEDED(hr) && *out;
}

#ifdef UIA_TextEditPatternId
bool QueryTextEditPattern(IUIAutomationElement *el, IUIAutomationTextEditPattern **out)
{
    *out = nullptr;
    IUnknown *pUnk = nullptr;
    HRESULT hr = el->GetCurrentPattern(UIA_TextEditPatternId, &pUnk);
    if (FAILED(hr) || !pUnk)
        return false;
    hr = pUnk->QueryInterface(IID_PPV_ARGS(out));
    pUnk->Release();
    return SUCCEEDED(hr) && *out;
}
#endif

bool RectsToCaretPixelRect(SAFEARRAY *psa, PixelRect &outRect)
{
    if (!psa || SafeArrayGetDim(psa) != 1)
        return false;

    LONG lb = 0, ub = 0;
    SafeArrayGetLBound(psa, 1, &lb);
    SafeArrayGetUBound(psa, 1, &ub);
    LONG count = ub - lb + 1;
    if (count < 4 || (count % 4) != 0)
        return false;
    LONG rects = count / 4;

    double *data = nullptr;
    if (FAILED(SafeArrayAccessData(psa, reinterpret_cast<void HUGEP **>(&data))) || !data)
        return false;

    double bestTop = 0.0, bestBottom = 0.0, bestLeft = 0.0, bestRight = 0.0;
    bool got = false;
    for (LONG r = 0; r < rects; ++r)
    {
        const double *v = data + r * 4;
        double left = v[0], top = v[1], width = v[2], height = v[3];
        if (height <= 0.0)
            continue;
        double bottom = top + height;
        if (!got || bottom > bestBottom)
        {
            got = true;
            bestTop = top;
            bestBottom = bottom;
            bestLeft = left;
            bestRight = left + std::max(0.0, width);
        }
    }
    SafeArrayUnaccessData(psa);
    if (!got)
        return false;

    outRect.left = static_cast<int32_t>(std::llround(bestLeft));
    outRect.top = static_cast<int32_t>(std::llround(bestTop));
    outRect.right = static_cast<int32_t>(std::llround(bestRight));
    outRect.bottom = static_cast<int32_t>(std::llround(bestBottom));
    if (outRect.right < outRect.left)
        outRect.right = outRect.left;
    return outRect.bottom > outRect.top;
}

// 仅在非密码框下，检查往前借 1 个字符后最后一个字符是否为换行符 (\r / \n)。
// 严禁把任何文本写入日志。
bool RangeToCaretPixelRect(IUIAutomationTextRange *pRange,
                           bool isPassword,
                           bool hasControlRect,
                           const PixelRect &controlRect,
                           PixelRect &outRect,
                           SidebandProtocol::CaretConfidence &outConf)
{
    if (!pRange)
        return false;

    // 1. 直接取边界矩形（非退化选区或 Chromium 直接返回 0 宽 + 行高）
    SAFEARRAY *psa = nullptr;
    if (SUCCEEDED(pRange->GetBoundingRectangles(&psa)))
    {
        bool ok = RectsToCaretPixelRect(psa, outRect);
        if (psa)
            SafeArrayDestroy(psa);
        if (ok)
        {
            outConf = SidebandProtocol::CaretConfidence::Exact;
            return true;
        }
    }

    // 2. 退化区间：ExpandToEnclosingUnit(TextUnit_Character) 归一化
    IUIAutomationTextRange *pExpanded = nullptr;
    if (SUCCEEDED(pRange->Clone(&pExpanded)) && pExpanded)
    {
        pExpanded->ExpandToEnclosingUnit(TextUnit_Character);
        psa = nullptr;
        if (SUCCEEDED(pExpanded->GetBoundingRectangles(&psa)))
        {
            bool ok = RectsToCaretPixelRect(psa, outRect);
            if (psa)
                SafeArrayDestroy(psa);
            if (ok)
            {
                pExpanded->Release();
                outConf = SidebandProtocol::CaretConfidence::High;
                return true;
            }
        }
        pExpanded->Release();
    }

    // 3. 行尾/文本末尾：向前回退借 1 个字符
    IUIAutomationTextRange *pBack = nullptr;
    if (SUCCEEDED(pRange->Clone(&pBack)) && pBack)
    {
        int moved = 0;
        if (SUCCEEDED(pBack->MoveEndpointByUnit(TextPatternRangeEndpoint_Start,
                                                TextUnit_Character, -1, &moved)) &&
            moved != 0)
        {
            psa = nullptr;
            if (SUCCEEDED(pBack->GetBoundingRectangles(&psa)))
            {
                PixelRect prevCharRect{};
                bool ok = RectsToCaretPixelRect(psa, prevCharRect);
                if (psa)
                    SafeArrayDestroy(psa);
                if (ok)
                {
                    bool adjustedNewline = false;
                    if (!isPassword)
                    {
                        BSTR bstr = nullptr;
                        if (SUCCEEDED(pBack->GetText(2, &bstr)) && bstr)
                        {
                            UINT len = SysStringLen(bstr);
                            if (len > 0)
                            {
                                wchar_t ch = bstr[len - 1];
                                PixelRect nlRect{};
                                if (AdjustLineEndAfterNewline(prevCharRect,
                                                              controlRect.left + 4,
                                                              hasControlRect && !controlRect.IsEmpty(),
                                                              ch,
                                                              nlRect))
                                {
                                    outRect = nlRect;
                                    adjustedNewline = true;
                                }
                            }
                            SysFreeString(bstr);
                        }
                    }

                    if (!adjustedNewline)
                    {
                        // 非换行符末尾：把横坐标移到前一个字符右边沿
                        outRect = prevCharRect;
                        outRect.left = prevCharRect.right;
                        outRect.right = prevCharRect.right;
                    }

                    outConf = SidebandProtocol::CaretConfidence::High;
                    pBack->Release();
                    return true;
                }
            }
        }
        pBack->Release();
    }

    return false;
}

bool TryGetCachedControlRect(IUIAutomationElement *el, PixelRect &outRect)
{
    VARIANT vt;
    VariantInit(&vt);
    HRESULT hr = el->GetCachedPropertyValue(UIA_BoundingRectanglePropertyId, &vt);
    if (FAILED(hr) || vt.vt != (VT_ARRAY | VT_R8) || !vt.parray)
    {
        VariantClear(&vt);
        return false;
    }

    double *data = nullptr;
    LONG lb = 0, ub = 0;
    SafeArrayGetLBound(vt.parray, 1, &lb);
    SafeArrayGetUBound(vt.parray, 1, &ub);
    bool ok = false;
    if ((ub - lb + 1) >= 4 &&
        SUCCEEDED(SafeArrayAccessData(vt.parray, reinterpret_cast<void HUGEP **>(&data))) &&
        data)
    {
        double w = data[2];
        double h = data[3];
        if (w > 0.0 && h > 0.0)
        {
            outRect.left = static_cast<int32_t>(std::llround(data[0]));
            outRect.top = static_cast<int32_t>(std::llround(data[1]));
            outRect.right = static_cast<int32_t>(std::llround(data[0] + w));
            outRect.bottom = static_cast<int32_t>(std::llround(data[1] + h));
            ok = !outRect.IsEmpty();
        }
        SafeArrayUnaccessData(vt.parray);
    }
    VariantClear(&vt);
    return ok;
}

int GetCachedIntProp(IUIAutomationElement *el, PROPERTYID propId, int fallback)
{
    VARIANT vt;
    VariantInit(&vt);
    int res = fallback;
    if (SUCCEEDED(el->GetCachedPropertyValue(propId, &vt)) && vt.vt == VT_I4)
        res = vt.lVal;
    VariantClear(&vt);
    return res;
}

bool GetCachedBoolProp(IUIAutomationElement *el, PROPERTYID propId, bool fallback)
{
    VARIANT vt;
    VariantInit(&vt);
    bool res = fallback;
    if (SUCCEEDED(el->GetCachedPropertyValue(propId, &vt)) && vt.vt == VT_BOOL)
        res = (vt.boolVal != VARIANT_FALSE);
    VariantClear(&vt);
    return res;
}

std::wstring GetCachedStringProp(IUIAutomationElement *el, PROPERTYID propId)
{
    VARIANT vt;
    VariantInit(&vt);
    std::wstring res;
    if (SUCCEEDED(el->GetCachedPropertyValue(propId, &vt)) && vt.vt == VT_BSTR && vt.bstrVal)
        res.assign(vt.bstrVal, SysStringLen(vt.bstrVal));
    VariantClear(&vt);
    return res;
}

} // namespace

// ============================================================
// DPI 物理坐标换算
// ============================================================

bool ClientRectToScreenPhysical(HWND hwnd, const RECT &clientRc, PixelRect &outScreenRc)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    POINT tl{clientRc.left, clientRc.top};
    POINT br{clientRc.right, clientRc.bottom};

    const auto &dpi = DpiHelpers::Get();
    bool needLogicalToPhysical = false;
    DPI_AWARENESS_CONTEXT oldCtx = nullptr;
    bool switchedCtx = false;

    if (dpi.pGetWindowCtx && dpi.pGetAwareness && dpi.pSetThreadCtx && dpi.pLogicalToPhysical)
    {
        DPI_AWARENESS_CONTEXT winCtx = dpi.pGetWindowCtx(hwnd);
        if (winCtx && dpi.pGetAwareness(winCtx) != DPI_AWARENESS_PER_MONITOR_AWARE)
        {
            oldCtx = dpi.pSetThreadCtx(winCtx);
            switchedCtx = (oldCtx != nullptr);
            needLogicalToPhysical = true;
        }
    }

    BOOL ok1 = ClientToScreen(hwnd, &tl);
    BOOL ok2 = ClientToScreen(hwnd, &br);

    if (switchedCtx && dpi.pSetThreadCtx)
        dpi.pSetThreadCtx(oldCtx);

    if (!ok1 || !ok2)
        return false;

    if (needLogicalToPhysical && dpi.pLogicalToPhysical)
    {
        dpi.pLogicalToPhysical(hwnd, &tl);
        dpi.pLogicalToPhysical(hwnd, &br);
    }

    outScreenRc.left = tl.x;
    outScreenRc.top = tl.y;
    outScreenRc.right = br.x;
    outScreenRc.bottom = br.y;
    return true;
}

// ============================================================
// 上下文构建
// ============================================================

CaretContextSnapshot BuildCaretContext(const CaretContextSnapshot *eventHint)
{
    CaretContextSnapshot ctx;
    ctx.nowMs = GetTickCount64();
    if (eventHint)
    {
        ctx.triggeredByImeEvent = eventHint->triggeredByImeEvent;
        ctx.triggeredByCaretEvent = eventHint->triggeredByCaretEvent;
        ctx.imeVisible = eventHint->imeVisible;
        ctx.imeWindowRect = eventHint->imeWindowRect;
        ctx.lastImeEventMs = eventHint->lastImeEventMs;
        ctx.lastKeyboardInputMs = eventHint->lastKeyboardInputMs;
    }

    HWND fg = GetForegroundWindow();
    ctx.foregroundWindow = reinterpret_cast<uint64_t>(fg);
    if (fg)
    {
        RECT fgRc{};
        if (GetWindowRect(fg, &fgRc))
            ctx.foregroundWindowRect = ToPixelRect(fgRc);
        ctx.topClassName = SafeGetClassName(fg);
    }

    GUITHREADINFO gti{};
    gti.cbSize = sizeof(GUITHREADINFO);
    BOOL gotGti = GetGUIThreadInfo(0, &gti);
    DWORD tid = 0;
    DWORD pid = 0;
    if (fg)
        tid = GetWindowThreadProcessId(fg, &pid);

    if (!gotGti && tid != 0)
    {
        gti = GUITHREADINFO{};
        gti.cbSize = sizeof(GUITHREADINFO);
        gotGti = GetGUIThreadInfo(tid, &gti);
    }

    if (gotGti)
    {
        ctx.hasGuiInfo = true;
        ctx.guiFlags = gti.flags;
        ctx.isCaretBlinking = (gti.flags & GUI_CARETBLINKING) != 0;
        ctx.activeWindow = reinterpret_cast<uint64_t>(gti.hwndActive);
        ctx.caretWindow = reinterpret_cast<uint64_t>(gti.hwndCaret);
        ctx.caretClientRect = ToPixelRect(gti.rcCaret);

        HWND focus = gti.hwndFocus ? gti.hwndFocus : (gti.hwndActive ? gti.hwndActive : fg);
        ctx.focusedWindow = reinterpret_cast<uint64_t>(focus);
        if (focus)
        {
            DWORD fpid = 0;
            DWORD ftid = GetWindowThreadProcessId(focus, &fpid);
            if (fpid != 0)
                pid = fpid;
            if (ftid != 0)
                tid = ftid;

            ctx.focusClassName = SafeGetClassName(focus);
            RECT cr{};
            if (GetClientRect(focus, &cr))
                ctx.focusClientRect = ToPixelRect(cr);
            RECT wr{};
            if (GetWindowRect(focus, &wr))
                ctx.focusWindowRect = ToPixelRect(wr);

            if (IsClassicEditClass(ctx.focusClassName))
            {
                LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
                if (style & ES_PASSWORD)
                    ctx.isFocusPasswordStyle = true;
            }
        }
    }
    else if (fg)
    {
        ctx.focusedWindow = ctx.foregroundWindow;
        ctx.focusClassName = ctx.topClassName;
        ctx.focusWindowRect = ctx.foregroundWindowRect;
        RECT cr{};
        if (GetClientRect(fg, &cr))
            ctx.focusClientRect = ToPixelRect(cr);
    }

    ctx.processId = pid;
    ctx.threadId = tid;
    return ctx;
}

// ============================================================
// Level 1: Win32CaretProvider
// ============================================================

ProbeStatus Win32CaretProvider::Probe(const CaretContextSnapshot &ctx,
                                        const CaretOptions &opts,
                                        CaretResult &outResult)
{
    if (!ctx.hasGuiInfo || ctx.caretWindow == 0)
    {
        m_blinkTracker.Reset();
        return ProbeStatus::Miss;
    }

    HWND hwndCaret = reinterpret_cast<HWND>(ctx.caretWindow);
    if (!IsWindow(hwndCaret))
    {
        m_blinkTracker.Reset();
        return ProbeStatus::Miss;
    }

    RECT caretClientWinRc{};
    GetClientRect(hwndCaret, &caretClientWinRc);
    const PixelRect caretWinClientRect = ToPixelRect(caretClientWinRc);
    const bool isSelfDrawnHost =
        IsKnownSelfDrawnHostClass(ctx.focusClassName) ||
        IsKnownSelfDrawnHostClass(ctx.topClassName);

    const UINT blinkTime = GetCaretBlinkTime();
    const bool infiniteBlink = (blinkTime == INFINITE);

    PixelRect validClientRect{};
    if (!m_blinkTracker.Evaluate(ctx.isCaretBlinking,
                                 ctx.caretWindow,
                                 ctx.focusedWindow,
                                 ctx.foregroundWindow,
                                 ctx.caretClientRect,
                                 caretWinClientRect,
                                 isSelfDrawnHost,
                                 ctx.nowMs,
                                 opts.blinkGraceMs,
                                 infiniteBlink,
                                 validClientRect))
    {
        return ProbeStatus::Miss;
    }

    RECT srcRc{validClientRect.left, validClientRect.top,
               validClientRect.right, validClientRect.bottom};
    PixelRect screenRc{};
    if (!ClientRectToScreenPhysical(hwndCaret, srcRc, screenRc))
        return ProbeStatus::Miss;

    outResult = CaretResult{};
    outResult.rect = screenRc;
    outResult.hwnd = ctx.caretWindow;
    outResult.processId = ctx.processId;
    outResult.threadId = ctx.threadId;
    outResult.source = SidebandProtocol::CARET_SOURCE_WIN32;
    outResult.confidence = SidebandProtocol::CaretConfidence::Exact;
    outResult.valid = true;
    outResult.isTextInput = true;
    outResult.isPassword = ctx.isFocusPasswordStyle;
    outResult.timestampMs = ctx.nowMs;
    return ProbeStatus::Hit;
}

// ============================================================
// Level 2: AppMsgCaretProvider (Edit & Scintilla 无指针跨进程消息)
// ============================================================

ProbeStatus AppMsgCaretProvider::Probe(const CaretContextSnapshot &ctx, CaretResult &outResult)
{
    HWND hwnd = reinterpret_cast<HWND>(ctx.focusedWindow);
    if (!hwnd || !IsWindow(hwnd))
        return ProbeStatus::Miss;

    if (IsScintillaClass(ctx.focusClassName))
    {
        DWORD_PTR pos = 0;
        if (!SendMessageTimeoutW(hwnd, kSciGetCurrentPos, 0, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &pos))
            return ProbeStatus::HardFailure;

        DWORD_PTR px = 0, py = 0, lineH = 0;
        if (!SendMessageTimeoutW(hwnd, kSciPointXFromPosition, 0, static_cast<LPARAM>(pos),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &px) ||
            !SendMessageTimeoutW(hwnd, kSciPointYFromPosition, 0, static_cast<LPARAM>(pos),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &py))
            return ProbeStatus::HardFailure;

        SendMessageTimeoutW(hwnd, kSciTextHeight, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &lineH);
        int32_t h = (lineH >= 6 && lineH <= 160) ? static_cast<int32_t>(lineH) : 20;
        int32_t x = static_cast<int32_t>(static_cast<LONG_PTR>(px));
        int32_t y = static_cast<int32_t>(static_cast<LONG_PTR>(py));

        RECT clientRc{x, y, x + 2, y + h};
        PixelRect screenRc{};
        if (!ClientRectToScreenPhysical(hwnd, clientRc, screenRc))
            return ProbeStatus::Miss;

        outResult = CaretResult{};
        outResult.rect = screenRc;
        outResult.hwnd = ctx.focusedWindow;
        outResult.processId = ctx.processId;
        outResult.threadId = ctx.threadId;
        outResult.source = SidebandProtocol::CARET_SOURCE_APP_MSG;
        outResult.confidence = SidebandProtocol::CaretConfidence::Exact;
        outResult.valid = true;
        outResult.isTextInput = true;
        outResult.timestampMs = ctx.nowMs;
        return ProbeStatus::Hit;
    }

    if (IsEditMessageHostClass(ctx.focusClassName))
    {
        DWORD_PTR sel = 0;
        if (!SendMessageTimeoutW(hwnd, EM_GETSEL, 0, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &sel))
            return ProbeStatus::HardFailure;

        WORD caretCharIdx = HIWORD(static_cast<DWORD>(sel));
        DWORD_PTR ptPacked = 0;
        if (!SendMessageTimeoutW(hwnd, EM_POSFROMCHAR, static_cast<WPARAM>(caretCharIdx), 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &ptPacked))
            return ProbeStatus::HardFailure;

        if (static_cast<LONG_PTR>(ptPacked) == -1)
            return ProbeStatus::Miss;

        int32_t x = GET_X_LPARAM(static_cast<LPARAM>(ptPacked));
        int32_t y = GET_Y_LPARAM(static_cast<LPARAM>(ptPacked));
        int32_t h = 18;
        if (!ctx.focusClientRect.IsEmpty() && ctx.focusClientRect.Height() < 40)
            h = std::max<int32_t>(12, ctx.focusClientRect.Height() - 4);

        RECT clientRc{x, y, x + 2, y + h};
        PixelRect screenRc{};
        if (!ClientRectToScreenPhysical(hwnd, clientRc, screenRc))
            return ProbeStatus::Miss;

        outResult = CaretResult{};
        outResult.rect = screenRc;
        outResult.hwnd = ctx.focusedWindow;
        outResult.processId = ctx.processId;
        outResult.threadId = ctx.threadId;
        outResult.source = SidebandProtocol::CARET_SOURCE_APP_MSG;
        outResult.confidence = SidebandProtocol::CaretConfidence::Exact;
        outResult.valid = true;
        outResult.isTextInput = true;
        outResult.isPassword = ctx.isFocusPasswordStyle;
        outResult.timestampMs = ctx.nowMs;
        return ProbeStatus::Hit;
    }

    return ProbeStatus::Miss;
}

// ============================================================
// Level 3: MSAACaretProvider (Source = 2)
// ============================================================

ProbeStatus MSAACaretProvider::Probe(const CaretContextSnapshot &ctx,
                                       HWND eventHwnd,
                                       LONG eventIdObject,
                                       LONG eventIdChild,
                                       CaretResult &outResult)
{
    IAccessible *acc = nullptr;
    VARIANT varChild;
    VariantInit(&varChild);

    bool got = false;
    HWND targetHwnd = reinterpret_cast<HWND>(ctx.focusedWindow);

    if (ctx.triggeredByCaretEvent && eventHwnd && eventIdObject == OBJID_CARET && IsWindow(eventHwnd))
    {
        if (SUCCEEDED(AccessibleObjectFromEvent(eventHwnd, eventIdObject, eventIdChild, &acc, &varChild)) && acc)
        {
            got = true;
            targetHwnd = eventHwnd;
        }
    }

    if (!got && targetHwnd && IsWindow(targetHwnd))
    {
        if (SUCCEEDED(AccessibleObjectFromWindow(targetHwnd, OBJID_CARET, IID_IAccessible,
                                                 reinterpret_cast<void **>(&acc))) &&
            acc)
        {
            varChild.vt = VT_I4;
            varChild.lVal = CHILDID_SELF;
            got = true;
        }
    }

    if (!got || !acc)
    {
        VariantClear(&varChild);
        return ProbeStatus::Miss;
    }

    // 检查是否不可见（如系统隐藏/假光标）
    VARIANT vtState;
    VariantInit(&vtState);
    bool invisible = false;
    if (SUCCEEDED(acc->get_accState(varChild, &vtState)) && vtState.vt == VT_I4)
    {
        if (vtState.lVal & STATE_SYSTEM_INVISIBLE)
            invisible = true;
    }
    VariantClear(&vtState);

    long x = 0, y = 0, w = 0, h = 0;
    HRESULT hrLoc = invisible ? E_FAIL : acc->accLocation(&x, &y, &w, &h, varChild);
    acc->Release();
    VariantClear(&varChild);

    if (invisible || hrLoc != S_OK || h <= 0)
        return ProbeStatus::Miss;

    // 过滤落在客户区左上角附近的 1x1 / 超小占位光标
    if (w <= 2 && h < 6)
        return ProbeStatus::Miss;

    const PixelRect screenRc{x, y, x + std::max<long>(0, w), y + h};
    if (IsKnownSelfDrawnHostClass(ctx.focusClassName) || IsKnownSelfDrawnHostClass(ctx.topClassName))
    {
        if (h < 8 && w <= 2)
            return ProbeStatus::Miss;
    }

    outResult = CaretResult{};
    outResult.rect = screenRc;
    outResult.hwnd = reinterpret_cast<uint64_t>(targetHwnd);
    outResult.processId = ctx.processId;
    outResult.threadId = ctx.threadId;
    outResult.source = SidebandProtocol::CARET_SOURCE_MSAA;
    outResult.confidence = SidebandProtocol::CaretConfidence::Medium;
    outResult.valid = true;
    outResult.isTextInput = true;
    outResult.isPassword = ctx.isFocusPasswordStyle;
    outResult.timestampMs = ctx.nowMs;
    return ProbeStatus::Hit;
}

// ============================================================
// Level 4: UIACaretProvider (Source = 3)
// ============================================================

UIACaretProvider::~UIACaretProvider()
{
    Shutdown();
}

void UIACaretProvider::Shutdown()
{
    if (m_cacheReq)
    {
        m_cacheReq->Release();
        m_cacheReq = nullptr;
    }
    if (m_uia)
    {
        m_uia->Release();
        m_uia = nullptr;
    }
    if (m_comInited)
    {
        CoUninitialize();
        m_comInited = false;
    }
}

bool UIACaretProvider::EnsureInitialized()
{
    if (m_uia)
        return true;
    if (m_broken)
    {
        if (GetTickCount64() < m_nextRetryMs)
            return false;
        m_broken = false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        m_broken = true;
        m_nextRetryMs = GetTickCount64() + 60000ULL;
        return false;
    }
    if (SUCCEEDED(hr))
        m_comInited = true;

    // 优先 CUIAutomation8 以获取 IUIAutomation2 超时控制
    hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia));
    if (FAILED(hr) || !m_uia)
    {
        hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia));
    }

    if (FAILED(hr) || !m_uia)
    {
        char hrBuf[32];
        std::snprintf(hrBuf, sizeof(hrBuf), "0x%08lX", static_cast<unsigned long>(hr));
        Logger::Get().Warning("CaretEngine: UIA 初始化失败 hr=", hrBuf);
        m_uia = nullptr;
        m_broken = true;
        m_nextRetryMs = GetTickCount64() + 60000ULL;
        return false;
    }

    // 抗挂起超时保护（IUIAutomation2）
    IUIAutomation2 *uia2 = nullptr;
    if (SUCCEEDED(m_uia->QueryInterface(IID_PPV_ARGS(&uia2))) && uia2)
    {
        uia2->put_ConnectionTimeout(150);
        uia2->put_TransactionTimeout(200);
        uia2->Release();
    }

    // 构建单次 IPC 批量缓存请求
    if (SUCCEEDED(m_uia->CreateCacheRequest(&m_cacheReq)) && m_cacheReq)
    {
        m_cacheReq->AddProperty(UIA_ControlTypePropertyId);
        m_cacheReq->AddProperty(UIA_ClassNamePropertyId);
        m_cacheReq->AddProperty(UIA_IsPasswordPropertyId);
        m_cacheReq->AddProperty(UIA_ValueIsReadOnlyPropertyId);
        m_cacheReq->AddProperty(UIA_BoundingRectanglePropertyId);
        m_cacheReq->AddProperty(UIA_IsTextPattern2AvailablePropertyId);
        m_cacheReq->AddProperty(UIA_IsTextPatternAvailablePropertyId);
        m_cacheReq->AddProperty(UIA_IsValuePatternAvailablePropertyId);
#ifdef UIA_IsTextEditPatternAvailablePropertyId
        m_cacheReq->AddProperty(UIA_IsTextEditPatternAvailablePropertyId);
#endif
    }

    Logger::Get().Info("CaretEngine: UIA (CUIAutomation8) 初始化成功");
    return true;
}

void UIACaretProvider::CheckAndLogElevatedTarget(uint32_t targetPid)
{
    if (targetPid == 0 || targetPid == GetCurrentProcessId())
        return;

    static bool s_selfChecked = false;
    static bool s_selfElevated = false;
    if (!s_selfChecked)
    {
        s_selfChecked = true;
        HANDLE hSelfTok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hSelfTok))
        {
            TOKEN_ELEVATION elev{};
            DWORD retLen = 0;
            if (GetTokenInformation(hSelfTok, TokenElevation, &elev, sizeof(elev), &retLen))
                s_selfElevated = (elev.TokenIsElevated != 0);
            CloseHandle(hSelfTok);
        }
    }
    if (s_selfElevated)
        return;

    static std::set<uint32_t> s_warnedPids;
    if (s_warnedPids.count(targetPid))
        return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (!hProc)
        return;

    bool targetElevated = false;
    HANDLE hTok = nullptr;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &hTok))
    {
        TOKEN_ELEVATION elev{};
        DWORD retLen = 0;
        if (GetTokenInformation(hTok, TokenElevation, &elev, sizeof(elev), &retLen))
            targetElevated = (elev.TokenIsElevated != 0);
        CloseHandle(hTok);
    }
    else if (GetLastError() == ERROR_ACCESS_DENIED)
    {
        targetElevated = true;
    }
    CloseHandle(hProc);

    if (targetElevated)
    {
        s_warnedPids.insert(targetPid);
        Logger::Get().Warning("CaretEngine: 目标进程 pid=", targetPid,
                              " 运行在管理员/高权限(UIPI 隔离)，普通权限下可能无法读取其光标");
    }
}

void UIACaretProvider::MaybeWakeBrowserAccessibility(const CaretContextSnapshot &ctx,
                                                     const std::wstring &uiaClass,
                                                     CaretAffinityCache::Entry &affinityEntry)
{
    if (!IsLazyAccessibilityClass(ctx.focusClassName) &&
        !IsLazyAccessibilityClass(ctx.topClassName) &&
        !IsLazyAccessibilityClass(uiaClass))
        return;
    if (ctx.nowMs < affinityEntry.lastBrowserWakeMs + 2000ULL)
        return;

    HWND hwnd = reinterpret_cast<HWND>(ctx.focusedWindow ? ctx.focusedWindow : ctx.foregroundWindow);
    if (!hwnd || !IsWindow(hwnd))
        return;

    affinityEntry.lastBrowserWakeMs = ctx.nowMs;
    DWORD_PTR dummy = 0;
    SendMessageTimeoutW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CLIENT),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &dummy);
}

ProbeStatus UIACaretProvider::Probe(const CaretContextSnapshot &ctx,
                                    CaretAffinityCache::Entry &affinityEntry,
                                    CaretResult &outResult,
                                    CaretResult *outControlBoxFallback)
{
    if (!EnsureInitialized())
        return ProbeStatus::HardFailure;

    IUIAutomationElement *pFocus = nullptr;
    HRESULT hr = m_cacheReq
                     ? m_uia->GetFocusedElementBuildCache(m_cacheReq, &pFocus)
                     : m_uia->GetFocusedElement(&pFocus);

    if (FAILED(hr) || !pFocus)
    {
        if (hr == static_cast<HRESULT>(0x8001011FL) || hr == static_cast<HRESULT>(0x800705B4L))
            return ProbeStatus::HardFailure;
        return ProbeStatus::Miss;
    }

    int ctlType = -1;
    bool isPassword = ctx.isFocusPasswordStyle;
    bool isReadOnly = false;
    std::wstring uiaClass;
    PixelRect controlRect{};
    bool hasControlRect = false;
    bool hasTP2 = true;
    bool hasTP = true;
    bool hasVP = false;
#ifdef UIA_TextEditPatternId
    bool hasTE = true;
#endif

    if (m_cacheReq)
    {
        ctlType = GetCachedIntProp(pFocus, UIA_ControlTypePropertyId, -1);
        isPassword = isPassword || GetCachedBoolProp(pFocus, UIA_IsPasswordPropertyId, false);
        isReadOnly = GetCachedBoolProp(pFocus, UIA_ValueIsReadOnlyPropertyId, false);
        uiaClass = GetCachedStringProp(pFocus, UIA_ClassNamePropertyId);
        hasControlRect = TryGetCachedControlRect(pFocus, controlRect);
        hasTP2 = GetCachedBoolProp(pFocus, UIA_IsTextPattern2AvailablePropertyId, false);
        hasTP = GetCachedBoolProp(pFocus, UIA_IsTextPatternAvailablePropertyId, false);
        hasVP = GetCachedBoolProp(pFocus, UIA_IsValuePatternAvailablePropertyId, false);
#ifdef UIA_IsTextEditPatternAvailablePropertyId
        hasTE = GetCachedBoolProp(pFocus, UIA_IsTextEditPatternAvailablePropertyId, false);
#endif
    }
    else
    {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(pFocus->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &vt)) && vt.vt == VT_I4)
            ctlType = vt.lVal;
        VariantClear(&vt);
    }

    bool ok = false;
    bool isComposition = false;
    bool isSelection = false;
    PixelRect caretRect{};
    auto conf = SidebandProtocol::CaretConfidence::None;

    // 1. 优先尝试 IME 组合输入态：TextEditPattern::GetActiveComposition / GetConversionTarget
#ifdef UIA_TextEditPatternId
    if (hasTE)
    {
        IUIAutomationTextEditPattern *pEditPat = nullptr;
        if (QueryTextEditPattern(pFocus, &pEditPat))
        {
            IUIAutomationTextRange *pCompRange = nullptr;
            if (SUCCEEDED(pEditPat->GetActiveComposition(&pCompRange)) && pCompRange)
            {
                if (RangeToCaretPixelRect(pCompRange, isPassword, hasControlRect, controlRect,
                                          caretRect, conf))
                {
                    ok = true;
                    isComposition = true;
                }
                pCompRange->Release();
            }
            if (!ok)
            {
                IUIAutomationTextRange *pConvRange = nullptr;
                if (SUCCEEDED(pEditPat->GetConversionTarget(&pConvRange)) && pConvRange)
                {
                    if (RangeToCaretPixelRect(pConvRange, isPassword, hasControlRect, controlRect,
                                              caretRect, conf))
                    {
                        ok = true;
                        isComposition = true;
                    }
                    pConvRange->Release();
                }
            }
            pEditPat->Release();
        }
    }
#endif

    // 2. 常规精确光标：TextPattern2::GetCaretRange (必须校验 isActive == TRUE)
    if (!ok && hasTP2)
    {
        IUIAutomationTextPattern2 *pPattern2 = nullptr;
        if (QueryTextPattern2(pFocus, &pPattern2))
        {
            BOOL isActive = FALSE;
            IUIAutomationTextRange *pRange = nullptr;
            if (SUCCEEDED(pPattern2->GetCaretRange(&isActive, &pRange)) && pRange)
            {
                if (isActive &&
                    RangeToCaretPixelRect(pRange, isPassword, hasControlRect, controlRect,
                                          caretRect, conf))
                {
                    ok = true;
                }
                pRange->Release();
            }
            pPattern2->Release();
        }
    }

    // 3. 退化路径：TextPattern::GetSelection
    //    - Edit / ComboBox / Windows Terminal 直接放行
    //    - 非浏览器窗口的 Document / Custom（如 WPF RichTextBox / Avalonia），仅在非只读且选区折叠(Start==End)时放行
    if (!ok && hasTP)
    {
        const bool isBrowser = IsBrowserOrWebViewClass(ctx.focusClassName) ||
                               IsBrowserOrWebViewClass(ctx.topClassName) ||
                               IsBrowserOrWebViewClass(uiaClass);
        const bool directAllowed =
            (ctlType == UIA_EditControlTypeId) ||
            (ctlType == UIA_ComboBoxControlTypeId) ||
            IsWindowsTerminalClass(ctx.topClassName);
        const bool collapsedDocAllowed =
            !isBrowser && !isReadOnly &&
            (ctlType == UIA_DocumentControlTypeId || ctlType == UIA_CustomControlTypeId);

        if (directAllowed || collapsedDocAllowed)
        {
            IUIAutomationTextPattern *pPattern = nullptr;
            if (QueryTextPattern(pFocus, &pPattern))
            {
                IUIAutomationTextRangeArray *pRanges = nullptr;
                if (SUCCEEDED(pPattern->GetSelection(&pRanges)) && pRanges)
                {
                    int n = 0;
                    if (SUCCEEDED(pRanges->get_Length(&n)))
                    {
                        for (int i = 0; i < n && !ok; ++i)
                        {
                            IUIAutomationTextRange *pRange = nullptr;
                            if (FAILED(pRanges->GetElement(i, &pRange)) || !pRange)
                                continue;
                            if (!directAllowed)
                            {
                                int cmp = 1;
                                if (FAILED(pRange->CompareEndpoints(TextPatternRangeEndpoint_Start,
                                                                    pRange,
                                                                    TextPatternRangeEndpoint_End,
                                                                    &cmp)) ||
                                    cmp != 0)
                                {
                                    pRange->Release();
                                    continue;
                                }
                            }
                            if (RangeToCaretPixelRect(pRange, isPassword, hasControlRect, controlRect,
                                                      caretRect, conf))
                            {
                                ok = true;
                                isSelection = true;
                            }
                            pRange->Release();
                        }
                    }
                    pRanges->Release();
                }
                pPattern->Release();
            }
        }
    }

    // 4. 输入控件矩形兜底（ControlBox，不伪装为精确光标；覆盖空 WPF/Avalonia TextBox、PasswordBox 及自定义可编辑控件）
    const bool isBrowserCtx = IsBrowserOrWebViewClass(ctx.focusClassName) ||
                              IsBrowserOrWebViewClass(ctx.topClassName) ||
                              IsBrowserOrWebViewClass(uiaClass);
    const bool editableControlType =
        (ctlType == UIA_EditControlTypeId) ||
        (ctlType == UIA_ComboBoxControlTypeId) ||
        (!isBrowserCtx && !isReadOnly &&
         (ctlType == UIA_DocumentControlTypeId ||
          (ctlType == UIA_CustomControlTypeId && hasVP)));

    if (!ok && outControlBoxFallback && editableControlType &&
        hasControlRect && !controlRect.IsEmpty())
    {
        PixelRect boxRect{};
        bool boxValid = false;
        const int32_t ch = controlRect.Height();
        if (ch >= 14 && ch <= 110)
        {
            boxRect = controlRect;
            boxValid = true;
        }
        else if (ch > 110 && ch <= 900 && !isBrowserCtx && !isReadOnly &&
                 (ctlType == UIA_EditControlTypeId || ctlType == UIA_DocumentControlTypeId))
        {
            // 多行空文本框（如 WPF/Avalonia 空置多行输入框，TextPattern 无字符可返回）：估算首行输入区域
            boxRect = PixelRect{
                controlRect.left,
                controlRect.top + 4,
                controlRect.left + 2,
                std::min<int32_t>(controlRect.bottom, controlRect.top + 28)
            };
            boxValid = (boxRect.Height() >= 10);
        }

        if (boxValid)
        {
            CaretResult box;
            box.rect = boxRect;
            box.controlRect = controlRect;
            box.hasControlRect = true;
            box.hwnd = ctx.focusedWindow;
            box.processId = ctx.processId;
            box.threadId = ctx.threadId;
            box.source = SidebandProtocol::CARET_SOURCE_CONTROL_BOX;
            box.confidence = SidebandProtocol::CaretConfidence::Estimated;
            box.valid = true;
            box.isTextInput = true;
            box.isPassword = isPassword;
            box.timestampMs = ctx.nowMs;
            *outControlBoxFallback = box;
        }
    }

    pFocus->Release();

    if (!ok)
    {
        // 若是 Chromium/Electron/WebView/Qt 宿主，尝试发送 WM_GETOBJECT(OBJID_CLIENT) 唤醒惰性无障碍树
        if (IsLazyAccessibilityClass(ctx.focusClassName) ||
            IsLazyAccessibilityClass(ctx.topClassName) ||
            IsLazyAccessibilityClass(uiaClass))
        {
            MaybeWakeBrowserAccessibility(ctx, uiaClass, affinityEntry);
        }

        static std::set<std::string> reported;
        char key[96];
#ifdef UIA_TextEditPatternId
        const int teFlag = hasTE ? 1 : 0;
#else
        const int teFlag = 0;
#endif
        std::snprintf(key, sizeof(key), "ctl=%d tp2=%d tp=%d te=%d",
                      ctlType, hasTP2 ? 1 : 0, hasTP ? 1 : 0, teFlag);
        if (reported.insert(key).second)
        {
            Logger::Get().Debug("[文本光标] UIA 未取到精确 caret：", key);
        }
        return ProbeStatus::Miss;
    }

    outResult = CaretResult{};
    outResult.rect = caretRect;
    outResult.controlRect = controlRect;
    outResult.hasControlRect = hasControlRect;
    outResult.hwnd = ctx.focusedWindow;
    outResult.processId = ctx.processId;
    outResult.threadId = ctx.threadId;
    outResult.source = SidebandProtocol::CARET_SOURCE_UIA;
    outResult.confidence = conf;
    outResult.valid = true;
    outResult.isTextInput = true;
    outResult.isPassword = isPassword;
    outResult.isComposition = isComposition;
    outResult.isSelection = isSelection;
    outResult.timestampMs = ctx.nowMs;
    return ProbeStatus::Hit;
}

namespace {

struct TextHostEnumState {
    std::vector<HWND> hostWindows;
};

BOOL CALLBACK EnumTextInputHostProc(HWND hwnd, LPARAM lp)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return TRUE;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc)
        return TRUE;

    wchar_t exePath[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    bool isTextHost = false;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &sz) && sz > 0)
    {
        const wchar_t *slash = std::wcsrchr(exePath, L'\\');
        const wchar_t *name = slash ? (slash + 1) : exePath;
        if (_wcsicmp(name, L"TextInputHost.exe") == 0)
            isTextHost = true;
    }
    CloseHandle(hProc);

    if (isTextHost)
        reinterpret_cast<TextHostEnumState *>(lp)->hostWindows.push_back(hwnd);
    return TRUE;
}

} // namespace

bool UIACaretProvider::FindImeCandidateRect(PixelRect &outRect)
{
    if (!EnsureInitialized())
        return false;

    TextHostEnumState st;
    EnumWindows(EnumTextInputHostProc, reinterpret_cast<LPARAM>(&st));

    if (st.hostWindows.empty())
        return false;

    VARIANT vtList;
    VariantInit(&vtList);
    vtList.vt = VT_I4;
    vtList.lVal = UIA_ListControlTypeId;
    IUIAutomationCondition *pCondList = nullptr;
    m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vtList, &pCondList);
    VariantClear(&vtList);
    if (!pCondList)
        return false;

    bool found = false;
    for (HWND hwnd : st.hostWindows)
    {
        IUIAutomationElement *pRoot = nullptr;
        if (FAILED(m_uia->ElementFromHandle(hwnd, &pRoot)) || !pRoot)
            continue;

        IUIAutomationElementArray *pArr = nullptr;
        if (SUCCEEDED(pRoot->FindAll(TreeScope_Descendants, pCondList, &pArr)) && pArr)
        {
            int len = 0;
            pArr->get_Length(&len);
            for (int i = 0; i < len; ++i)
            {
                IUIAutomationElement *pItem = nullptr;
                if (FAILED(pArr->GetElement(i, &pItem)) || !pItem)
                    continue;
                BOOL offscreen = TRUE;
                RECT rc{};
                if (SUCCEEDED(pItem->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
                    SUCCEEDED(pItem->get_CurrentBoundingRectangle(&rc)))
                {
                    int32_t w = rc.right - rc.left;
                    int32_t h = rc.bottom - rc.top;
                    if (w >= 40 && w <= 900 && h >= 20 && h <= 260)
                    {
                        outRect = PixelRect{rc.left, rc.top, rc.right, rc.bottom};
                        found = true;
                    }
                }
                pItem->Release();
                if (found)
                    break;
            }
            pArr->Release();
        }
        pRoot->Release();
        if (found)
            break;
    }
    pCondList->Release();
    return found;
}

// ============================================================
// Level 5: IMECaretProvider (Source = 5)
// ============================================================

ProbeStatus IMECaretProvider::Probe(const CaretContextSnapshot &ctx,
                                    const CaretOptions &opts,
                                    const PixelRect *overrideImeRect,
                                    bool bypassEventGate,
                                    CaretResult &outResult)
{
    if (!opts.allowImeFallback)
        return ProbeStatus::Miss;

    const bool recentIme =
        ctx.imeVisible &&
        ctx.lastImeEventMs > 0 &&
        ctx.nowMs >= ctx.lastImeEventMs &&
        (ctx.nowMs - ctx.lastImeEventMs) <= 8000ULL;

    if (!bypassEventGate && !recentIme)
        return ProbeStatus::Miss;

    const PixelRect &imeRc = (overrideImeRect && !overrideImeRect->IsEmpty())
                                 ? *overrideImeRect
                                 : ctx.imeWindowRect;
    if (imeRc.IsEmpty() || imeRc.Width() < 20 || imeRc.Height() < 14 ||
        imeRc.Width() > 900 || imeRc.Height() > 280)
        return ProbeStatus::Miss;

    // 根据候选窗在屏幕中的位置判断其位于输入行下方（常规）还是上方（贴近屏幕底部时上翻）
    POINT probePt{imeRc.CenterX(), imeRc.top};
    HMONITOR hMon = MonitorFromPoint(probePt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    int32_t anchorBottom = imeRc.top;
    if (GetMonitorInfoW(hMon, &mi))
    {
        if (imeRc.bottom >= mi.rcWork.bottom - 24 && imeRc.top > mi.rcWork.top + 80)
        {
            // 上翻候选框：实际输入行在候选框下方
            anchorBottom = std::min<int32_t>(mi.rcMonitor.bottom, imeRc.bottom + 24);
        }
    }

    constexpr int32_t kEstLineHeight = 24;
    PixelRect estRect{
        imeRc.left,
        anchorBottom - kEstLineHeight,
        imeRc.left + 2,
        anchorBottom
    };

    outResult = CaretResult{};
    outResult.rect = estRect;
    outResult.hwnd = ctx.focusedWindow ? ctx.focusedWindow : ctx.foregroundWindow;
    outResult.processId = ctx.processId;
    outResult.threadId = ctx.threadId;
    outResult.source = SidebandProtocol::CARET_SOURCE_IME_ANCHOR;
    outResult.confidence = SidebandProtocol::CaretConfidence::Estimated;
    outResult.valid = true;
    outResult.isTextInput = true;
    outResult.isComposition = true;
    outResult.isPassword = ctx.isFocusPasswordStyle;
    outResult.timestampMs = ctx.nowMs;
    return ProbeStatus::Hit;
}

} // namespace Caret
