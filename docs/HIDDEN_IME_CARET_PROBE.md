# 隐藏式 Windows 隐形输入法光标探针（UI-Less TSF TIP / IMM32）设计方案

本文记录如何通过实现一个**无界面、不拦截按键的 Windows 隐形输入法（UI-Less IME / TSF TIP）**，让自绘窗口/终端/游戏主动交出文本插入符（Caret）坐标给 `moonlight-sideband`，同时**不弹出任何 Windows 候选框**、也不影响 Android 客户端通过 `KEYEVENTF_UNICODE`（`VK_PACKET`）直接上屏中文。

---

## 一、背景与动机

### 1.1 为什么外进程探测会遇到天花板
当前 `CursorModule`（`CaretEngine`）已经覆盖：
- Win32 `GUITHREADINFO`（含暗灭期宽限与 `1x1` 假光标过滤）
- MSAA（`AccessibleObjectFromWindow` / `OBJID_CARET`）
- UI Automation（`TextEditPattern` / `TextPattern2` / 受控 `TextPattern` / `ControlBox`）
- `Scintilla` / `Edit` 窗口消息与 `TextInputHost.exe` 候选框锚点

但对于**自绘界面且不实现 UIA TextPattern 的应用**（例如 `WindTerm` 的 Qt 自绘终端画布、部分 `Unity` / DirectX 输入框、自定义 Skia/D2D 编辑器），外进程无从得知光标像素坐标；同时 Android Moonlight 是在手机本地组词后通过 `VK_PACKET` 直接向 Windows 发送 Unicode 字符，**不会触发 Windows 自带输入法弹出候选框**，因此也无法靠读 `TextInputHost.exe` 候选框位置兜底。

### 1.2 核心思路：为什么“隐形输入法”有效
Windows 系统自带拼音输入法之所以能把候选框精准贴在 `WindTerm` / 游戏输入框下方，是因为：
1. Windows（`msctf.dll` / `imm32.dll`）会**由系统合法将当前激活的输入法 DLL 加载进每个前台文本进程内部**。
2. 目标应用会主动通过 **TSF（`ITfTextStoreACP::GetTextExt`）** 或 **IMM32（`ImmSetCompositionWindow`）** 把当前插入符坐标交给输入法。

如果我们注册一个**只取坐标、不画任何 UI、且对所有按键全部放行**的“Moonlight 隐形输入法”，就可以：
- 拿到目标进程内部交出的精确屏幕坐标；
- **零候选框**、**零组合窗**，完全不干扰手机端直出中文与英文键入。

---

## 二、整体架构与数据流

与当前纯绿色单文件 `moonlight_sideband.exe` 不同，该方案由**隐形输入法 DLL（x64 + x86）**与**现有守护进程**两部分组成：

```text
[目标前台进程: WindTerm / Qt / Unity / 浏览器 / WPF ...]
   │  由 Windows 系统自动加载对应架构的隐形输入法 DLL
   ▼
[MoonlightImeProbe.dll (x64 / x86, UI-Less TSF TIP + IMM32 桥接)]
   ├─ 1. 监听焦点切换 & 文本/选区变化
   ├─ 2. 不截胡按键 (OnKeyDown/OnTestKeyDown 全部 pfEaten = FALSE)
   ├─ 3. 不弹任何 UI (TF_TMF_UIELEMENTENABLED / 去掉 ISC_SHOWUIALL)
   ├─ 4. 采集光标坐标：
   │     - TSF 路径: ITfContext::GetSelection -> ITfContextView::GetTextExt
   │     - IMM32 路径: ImmLockIMC 读 cfCompForm / cfCandForm -> ClientToScreen
   └─ 5. 写入带低权限 ACL 的共享内存或 Named Pipe
   ▼
[moonlight_sideband.exe -> CursorModule / CaretEngine]
   ├─ 校验 pid / hwnd 与时间戳新鲜度（仅采信当前前台窗口近期更新）
   └─ 计算 YPercent / Height -> 发送 CmdID=2 与 CmdID=102 (TEXT_CURSOR_EXT)
```

---

## 三、核心技术实现细则

### 3.1 路线 A（主路径）：TSF UI-Less Text Input Processor
现代桌面应用（`WindTerm`/Qt、Chromium/Electron、Edge/WebView2、WPF、UWP、新版 Win32）即使**未开始中文拼音组词**，也可以通过 TSF 查询当前选区/插入符坐标。

1. **COM 与 Profile 注册**
   - 实现 `ITfTextInputProcessorEx`（支持 `TF_TMAE_UIELEMENTENABLEDONLY` / 无界面模式）。
   - 安装时通过 `ITfInputProcessorProfileMgr::RegisterProfile` 注册为独立键盘/输入法配置（例如 `Moonlight Input Probe`）。
2. **按键全放行（不启动 Composition）**
   - 实现 `ITfKeyEventSink`：在 `OnTestKeyDown`、`OnKeyDown`、`OnTestKeyUp`、`OnKeyUp` 中直接设置 `*pfEaten = FALSE` 并返回 `S_OK`。
   - Android 端通过 `SendInput(KEYEVENTF_UNICODE)`（`VK_PACKET`）或常规 `VK_*` 发送的字符会**直接落入目标输入框**，不会触发任何 Windows 组词状态或候选窗。
3. **只读异步读取光标矩形**
   - 注册 `ITfThreadMgrEventSink`（感知焦点文档切换 `OnSetFocus`）与 `ITfTextEditSink`（感知文本或光标选区移动 `OnEndEdit`）。
   - 触发后在目标线程内调用 `pContext->RequestEditSession(..., TF_ES_READ | TF_ES_ASYNC)`。
   - 在 `DoEditSession(TfEditCookie ec)` 内执行：
     1. `pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)` 获取当前选区范围 `sel.range`（或将退化/空选区扩展 1 字符后取起止边界）。
     2. `pContextView->GetTextExt(ec, sel.range, &rc, &fClipped)` 直接获取屏幕物理像素坐标 `RECT rc`。

### 3.2 路线 B（兼容老应用/游戏）：IMM32 / `CtfIme*` 坐标拦截
部分老游戏、老版 `Unity` 或直接调用 IMM32 API 的控件不完整实现 `ITfTextStoreACP`，而是调用 `ImmSetCompositionWindow(hIMC, &cf)` / `ImmSetCandidateWindow(hIMC, &cand)`。

1. **隐身处理**
   - 处理 `WM_IME_SETCONTEXT` 时清除所有系统 IME UI 标志：`lParam &= ~ISC_SHOWUIALL`。
   - DLL 自身不创建或永远不 `ShowWindow` 任何候选窗/状态窗/组词窗。
2. **坐标采集**
   - 在 `CtfIme*` 桥接回调或 `NotifyIME` / `WM_IME_NOTIFY`（关注 `IMN_SETCOMPOSITIONWINDOW`、`IMN_SETCANDIDATEPOS`、`IMN_OPENCANDIDATE`）时调用 `ImmLockIMC(hIMC)`。
   - 读取 `INPUTCONTEXT::cfCompForm[0].ptCurrentPos`（优先）或 `cfCandForm[0].ptCurrentPos / rcArea`，通过 `ClientToScreen(hwnd, &pt)` 转为屏幕坐标。

### 3.3 跨进程 IPC 与沙盒权限（关键细节）
目标进程可能是浏览器渲染沙盒（Low IL / AppContainer）、普通进程（Medium IL）或提权进程（High IL）：

- **Named Pipe 或 File Mapping (`Local\` / `Global\`)** 在 `moonlight_sideband.exe` 启动时创建，**必须显式设置 `SECURITY_DESCRIPTOR` DACL**，允许：
  - `Authenticated Users` / `Everyone` 写入单向遥测数据；
  - **`ALL APPLICATION PACKAGES` (`S-1-15-2-1`)** 写入权限（否则 UWP、Edge/Chrome 沙盒、部分 WebView2 渲染进程无法上报坐标）。
- 载荷结构尽量极简定长（避免锁死目标进程 UI 线程，非阻塞投递）：
  ```cpp
  struct ImeCaretSample {
      uint32_t pid;
      uint64_t hwnd;       // 目标窗口句柄
      int32_t  left;
      int32_t  top;
      int32_t  right;
      int32_t  bottom;
      uint32_t kind;       // 1 = TSF GetTextExt, 2 = IMM32 CompForm, 3 = IMM32 CandForm
      uint64_t tickMs;     // GetTickCount64()
  };
  ```

### 3.4 串流期间自动切换与还原
- Android 客户端建立旁路/串流连接后，`moonlight_sideband.exe` 记录当前前台输入法配置，并向前台窗口发 `WM_INPUTLANGCHANGEREQUEST` 或调用 `ITfInputProcessorProfileMgr::ActivateProfile` 切换至 `Moonlight Input Probe`。
- 串流断开或 `moonlight_sideband.exe` 退出时，恢复用户原本激活的输入法，避免影响日常桌面使用。

---

## 四、安装、架构与系统约束

引入隐形输入法探针会改变当前项目的交付形态，实施前需确认以下条件：

1. **双架构编译（不再是单 EXE）**
   - 64 位进程只能加载 64 位输入法 DLL，32 位进程（老游戏、部分工具）只能加载 32 位 DLL，必须同时构建并注册 **`x64` 与 `x86`** 两个 DLL。
2. **目录权限与管理员注册**
   - 需以管理员权限把 DLL 安装到 `Program Files` 或系统目录，并赋予目录 `ALL APPLICATION PACKAGES (S-1-15-2-1)` 的“读取和执行”权限，随后注册 COM CLSID 与 TIP Profile。
3. **代码签名建议**
   - 正常桌面应用可配合测试签名或普通代码签名使用；若要让更多带保护的进程信任加载，建议使用受信任证书签名。

---

## 五、与 `CaretEngine` 和协议层的对接

- 在 `modules/cursor/CaretTypes.hpp` 与 `CaretProviders` 中可新增高优先级（或紧随 Win32 系统光标之后）的 `ImeProbeProvider`：
  - 从 IPC 缓存读取最近一次 `ImeCaretSample`；
  - 严格校验 `sample.hwnd == fgHwnd`（或同属前台窗口树 `ctx.pid == sample.pid`）且 `now - sample.tickMs <= 500ms`；
  - 通过 `IsCaretRectReasonable` 校验后，以 `CaretConfidence::Exact` 输出。
- 向下游协议映射：
  - 老协议 `CmdID=2` 照常输出 `YPercent` / `Height`；
  - 扩展协议 `CmdID=102`（`TEXT_CURSOR_EXT`）可复用现有字段并区分精确定位（`conf=5`）。

---

## 六、已知硬边界（即使做了隐形输入法仍不可覆盖）

为了避免无效投入，以下场景的限制必须在方案中明确标注：

1. **Windows 锁屏 / UAC 安全桌面（`LogonUI.exe`）**
   - 锁屏运行在隔离的 `Winlogon` 桌面，Windows 出于安全原因强制禁用第三方输入法加载，**依然无法读取**。
2. **内核级反作弊游戏（例如 Roblox 的 Hyperion/Byfron）**
   - 反作弊驱动/用户态防护会拦截未在系统白名单或未受信任微软签名的第三方输入法 DLL 加载；强行注入或对抗反作弊还存在封号风险，**不建议在强反作弊游戏中依赖本方案**。
3. **仅在组词期间才上报坐标的个别游戏**
   - 极少数自绘游戏只在收到 `WM_IME_STARTCOMPOSITION` 处于中文拼音组词态时才调用 `ImmSetCompositionWindow`。由于手机端通过 `VK_PACKET` 直接上屏不触发组词，这类游戏在纯直出模式下**仍可能不上报坐标**（不建议强行发送假 `StartComposition`/`CancelComposition`，容易导致游戏吞字或状态机错乱）。

---

## 七、推荐的最后一道兜底（Android 客户端联动）

结合桌面端硬边界，最稳妥的完整闭环是 **“PC 端探针尽可能精准上报 + Android 端兜底避让”**：
- PC 端（当前 `CaretEngine` + 可选的本隐形输入法探针）拿到坐标时，Android 按 `YPercent` 精确平移画面；
- 当 PC 端明确返回 **`YPercent == -1`**（如锁屏、Roblox、不报坐标的纯游戏画布）且 Android 软键盘弹出时，Android 端 (`moonlight-cjcxj`) 直接使用**用户刚才点击屏幕触发输入的最后一次触控 Y 坐标**，确保软键盘不挡住用户点击的输入区域。
