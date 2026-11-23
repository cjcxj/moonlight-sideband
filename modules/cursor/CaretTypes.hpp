#pragma once

/**
 * 文本插入符检测 —— 平台无关的数据模型与枚举定义
 *
 * 本文件刻意不引入 <windows.h>，以便纯逻辑测试（sideband_tests）
 * 可以在无 Windows SDK 的环境中直接编译验证。
 */

#include "SidebandProtocol.hpp"

#include <cstdint>
#include <string>

namespace Caret {

struct PixelRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t Width() const { return right - left; }
    int32_t Height() const { return bottom - top; }
    int32_t CenterX() const { return left + (right - left) / 2; }
    bool IsEmpty() const { return right <= left || bottom <= top; }
};

enum class ProviderId : uint8_t {
    Win32 = 0,
    AppMsg = 1,   // Scintilla / Edit / RichEdit 直接窗口消息
    MSAA = 2,     // OBJID_CARET / accLocation
    UIA = 3,      // TextEditPattern / TextPattern2 / gated TextPattern
    IME = 4,      // IME 组合/候选框锚点
    ControlBox = 5,
    Count = 6
};

struct CaretResult {
    PixelRect rect{};
    PixelRect controlRect{};
    bool hasControlRect = false;

    uint64_t hwnd = 0;       // 实际持有插入符/焦点的窗口句柄数值
    uint32_t processId = 0;
    uint32_t threadId = 0;

    int32_t source = SidebandProtocol::CARET_SOURCE_NONE;
    SidebandProtocol::CaretConfidence confidence = SidebandProtocol::CaretConfidence::None;

    bool valid = false;
    bool isTextInput = false;
    bool isPassword = false;     // 仅标记，严禁读取或记录任何密码文本内容
    bool isComposition = false;  // 是否处于 IME 组合输入态
    bool isSelection = false;    // 是否来自选区而非纯粹插入点

    uint64_t timestampMs = 0;    // GetTickCount64() 单调时钟
};

struct CaretContextSnapshot {
    uint64_t foregroundWindow = 0;
    uint64_t activeWindow = 0;
    uint64_t focusedWindow = 0;
    uint64_t caretWindow = 0;

    PixelRect caretClientRect{};
    PixelRect focusClientRect{};
    PixelRect focusWindowRect{};
    PixelRect foregroundWindowRect{};
    uint32_t guiFlags = 0;
    bool isCaretBlinking = false;
    bool hasGuiInfo = false;
    bool isFocusPasswordStyle = false;

    uint32_t processId = 0;
    uint32_t threadId = 0;
    std::wstring focusClassName;
    std::wstring topClassName;

    bool triggeredByImeEvent = false;
    bool triggeredByCaretEvent = false;
    bool imeVisible = false;
    PixelRect imeWindowRect{};
    uint64_t lastImeEventMs = 0;
    uint64_t lastKeyboardInputMs = 0;
    uint64_t nowMs = 0;
};

struct CaretOptions {
    bool allowImeFallback = true;
    bool allowControlBoxForLegacy = true;
    int32_t blinkGraceMs = 800;
    int32_t providerCooldownMs = 2000;
    int32_t failureThreshold = 3;
    int32_t yPercentDeadzone = 15;   // 0.15% 屏高死区（约 2px @ 1440p）
    int32_t heightDeadzonePx = 4;
};

} // namespace Caret
