# Windows 文本光标探测：覆盖场景与边界说明

本文整理 `moonlight-sideband` 在不同 Windows 应用框架、典型软件与极端场景下的文本插入符（Caret）获取能力、覆盖率预估，以及各兜底层的协同边界。

---

## 一、总体覆盖率预估（分层对比）

> 说明：以下比例按普通 Windows 桌面用户的日常应用与软件形态估算，且**假设 Android Moonlight 在手机端组词后通过 `KEYEVENTF_UNICODE`（`VK_PACKET`）直接向 Windows 输入中文**（即 Windows 上不会弹出微软拼音候选框）。

| 方案组合 | 覆盖场景预估 | 说明 |
|---|---|---|
| **仅原有双路**（Win32 + UIA `TextPattern2`） | ~55%–65% | 能抓记事本/浏览器/现代 WinUI，但遇到假光标、空白 WPF `TextBox`、`PasswordBox`、Qt/Avalonia/老控件易丢失 |
| **当前仓库方案（`CaretEngine` 多级探测，普通权限）** | **~80%–85%** | 已补齐假光标过滤、暗灭宽限、AppMsg（Scintilla/Edit）、MSAA、UIA `TextEditPattern`/`ControlBox` 兜底 |
| **当前方案 + 托盘「以管理员身份重启」** | **~88%–90%** | 解锁以管理员权限运行的工具（如 `Tai`、开 TUN 模式的 `v2rayN`、提权编辑器/终端） |
| **再叠加「隐形输入法探针（UI-Less TSF/IMM32）」**（规划，见 [设计方案](HIDDEN_IME_CARET_PROBE.md)） | **~93%–96%** | 进一步覆盖 `WindTerm` 等自绘 Qt 终端，以及在聚焦期会主动上报 `ImmSetCompositionWindow` 的部分桌面游戏/自绘框 |
| **再叠加 Android 端「`YPercent == -1` 时按最后触控点避让」** | **~99%+（视觉不遮挡）** | 对锁屏、Roblox、不报坐标的纯游戏画布实现兜底避让 |

---

## 二、按 Windows 应用框架 / 控件类型拆解

| 应用框架 / 类型 | 典型软件举例 | 当前 `CaretEngine` 命中路径 | 覆盖状态 | 说明与前提 |
|---|---|---|---|---|
| **Win32 原生 / MFC / 标准 Edit / RichEdit** | 记事本、资源管理器地址栏、传统对话框、搜索框 | `Win32` (`GUITHREADINFO`) / `AppMsg` (`EM_POSFROMCHAR`) / `MSAA` | ✅ 稳定覆盖 | 含 800ms 闪烁暗灭宽限；密码框只拿位置不读字符 |
| **Scintilla 编辑器** | Notepad++、部分轻量 IDE | `AppMsg` (`SCI_POINTXFROMPOSITION`) / `Win32` | ✅ 稳定覆盖 | 无跨进程指针，纯安全整数消息查询 |
| **Chromium / Electron / WebView2** | Chrome、Edge、VS Code、Discord、Slack、Notion、WebView2 应用 | `UIA`（`TextEditPattern` / `TextPattern2`）+ 惰性无障碍唤醒 | ✅ 稳定覆盖 | 依赖无障碍树唤醒（`WM_GETOBJECT`，`OBJID_CLIENT`）；同一窗口条目 2 秒内不重复唤醒，单次调用 60ms 超时 |
| **Firefox / Gecko** | Firefox、Thunderbird | `UIA` / `MSAA`（`OBJID_CARET`） | ✅ 稳定覆盖 | 过滤 `1x1` IME 假光标避免抢占高优先级 |
| **UWP / WinUI 3 / Windows Terminal** | Win11 设置、新版记事本、Windows Terminal、PowerToys | `UIA`（`TextPattern2` / 退化选区扩展 / `TextPattern`） | ✅ 稳定覆盖 | 处理了零宽选区与行尾换行回退借 1 字符 |
| **WPF / WinForms** | `Tai`、大量 C# 桌面工具、部分管理端 | `Win32` / `UIA`（`TextPattern` / 父子穿透 + `ControlBox`） | ✅ 需同权限 | 空白带占位符 `TextBox` 与 `PasswordBox` 走 `ControlBox`；自定义外壳聚焦时自动穿透父/子节点；若目标提权需开启 sideband 管理员模式 |
| **Avalonia / Uno / 自绘桌面面板** | `v2rayN`（v7+ Avalonia 版）、现代跨平台客户端 | `UIA`（父子穿透 + `Edit`/`Document`/`ValuePattern` `ControlBox`） | ✅ 需同权限 | 兼容 `TextPresenter` 内层或 `Window` 容器抢焦点场景；需确保旧 ini 里 `caret_allow_control_box=true`，开启 TUN 提权时需 sideband 同级提权 |
| **Java Swing / AWT / JetBrains** | IntelliJ IDEA、PyCharm、CLion | `Win32`（部分版本）/ `IMEAnchor`（本地打字时） | ⚠️ 部分覆盖 | JetBrains JBR 新版通常带系统 Win32 虚拟插入符或 TSF；旧版未开 JAB 时依赖 IME 或未来隐形输入法 |
| **Qt 经典控件（`QLineEdit` / `QTextEdit`）** | 常见 Qt 客户端表单框 | `Win32` / `UIA` / `MSAA` + 惰性唤醒 | ⚠️ 视版本号与模板 | Qt 5/6 标准文本框大多有 UIA；若为自定义 QPainter 画布则退化为不可见 |
| **Qt 自绘终端 / GPU 画布** | **`WindTerm`** | 外进程无文本接口 | ❌ 当前盲区 | 自绘网格，不暴露 Win32 Caret / MSAA / UIA；需**隐形输入法探针（TSF）**或 Android 触控兜底 |
| **独立/普通游戏引擎输入框** | **Unity（`InputField` / `TMP_InputField`）**、Godot、SDL | 外进程仅能在系统弹出 IME 候选框时靠 `IMEAnchor` | ⚠️ 仅本地拼音有效 | 手机端 `VK_PACKET` 直出中文不弹 Windows 候选框；需靠游戏聚焦时调用 `ImmSetCompositionWindow`（隐形输入法）或 Android 触控兜底 |
| **强反作弊客户端 / 网游** | **Roblox**（Hyperion / Byfron）及同类内核级反作弊游戏 | 被反作弊与无接口双重阻断 | ❌ 硬边界 | 不建议也不应尝试 DLL 注入或绕过反作弊；依赖 Android 端最后点击位置避让 |
| **Windows 锁屏 / 登录 / UAC 安全桌面** | `LogonUI.exe`、UAC 弹窗 | 系统 `Winlogon` 桌面隔离 | ❌ 系统硬边界 | 用户会话进程（即使管理员）无权跨桌面访问，且系统禁用第三方输入法；依赖 Android 端点击避让 |

---

## 三、实测典型失败场景归因与应对表

针对真实桌面环境下容易拿不到坐标的 6 类代表性目标，具体原因与现状如下：

| 实测目标 | 根因分析 | 当前版本能否解决 | 完整解决路径 |
|---|---|---|---|
| **1. `Tai`** | 可能以**管理员权限**运行产生 **UIPI 隔离**；弹窗空白 `TextBox`（含占位符）无 `TextPattern2`，部分自定义壳会让 UIA 焦点落在外壳或内嵌文本节点 | ✅ **同权限 + UIA 穿透可解** | 若提权则托盘右键 **「以管理员身份重启」**；确认 `caret_allow_control_box=true`，已补充 UIA 父/子穿透与 `ControlBox` 兜底 |
| **2. `v2rayN`（部分界面）** | v7+ 采用 **Avalonia** 自绘，空框（含灰字水印）无 `TextPattern2`，UIA `GetFocusedElement` 可能落在内层 `TextPresenter` 或外层 `Window`；旧 `.ini` 可能残留 `caret_allow_control_box=false`；开 TUN 后会提权 | ✅ **默认配置 + UIA 穿透可解** | 检查 `moonlight_sideband.ini` 中 `caret_allow_control_box=true`；已加 Avalonia 唤醒、父/子焦点查找与 `SelectValidationWindowBound`；开 TUN 需托盘提权 |
| **3. `WindTerm`** | 基于 Qt 自绘终端格子（`QPainter` / OpenGL），不向 Win32 / MSAA / UIA 暴露文本选区或插入符矩形 | ❌ 外进程无解 | 进阶：接入[隐形输入法探针](HIDDEN_IME_CARET_PROBE.md)走 TSF `GetTextExt`；日常：Android 端 `-1` 时按最后触控点避让 |
| **4. `Unity` 游戏** | DirectX/Vulkan 自绘渲染，完全无 Win32/UIA 文本树；在手机端 `VK_PACKET` 直出中文时不弹微软候选框，导致 `IMEAnchor` 也不触发 | ❌ 外进程无解（直出模式下） | 若游戏在输入框聚焦期会主动更新 `ImmSetCompositionWindow`，可用[隐形输入法探针](HIDDEN_IME_CARET_PROBE.md)；若仅组词期更新则走 Android 最后点击点兜底 |
| **5. `Roblox` 应用及游戏** | 3D 自绘画布 + **Hyperion/Byfron 反作弊**阻断外部读取与未签名第三方 DLL | ❌ 硬边界 | 不做注入/对抗，由 Android 端在 `YPercent == -1` 时按用户最后一次触控 Y 坐标自动上移画面 |
| **6. Windows 锁屏密码界面** | 运行在内核隔离的 `Winlogon` 桌面（`LogonUI.exe`），普通桌面会话进程与第三方输入法均无法进入 | ❌ 系统硬边界 | 同上，由 Android 端在 `YPercent == -1` 且软键盘弹出时按最后触控位置避让 |

---

## 四、使用与排查要点

### 4.1 配置检查（`moonlight_sideband.ini`）
若从早期版本升级而来，已有配置文件里可能残留旧默认值，建议确认以下两项为 `true`：

```ini
caret_allow_ime_fallback=true
caret_allow_control_box=true
```

### 4.2 提权软件与 UIA 未命中排查
1. **看启动配置日志**：启动后会打印 `CursorModule: 已启动 (ime_fallback=..., control_box=...)`（`[INFO]`，默认级别就能看到），确认旧版 `.ini` 未把 `control_box` 关掉；命中输入框却被配置拦截时，会额外记录一条明确的 `[WARN]`。
2. **提权（UIPI）提示不用开 `DEBUG`**：日志里出现下面这行（`[WARN]`，默认 `log_level=INFO` 就能看到），即在托盘右键点 **「以管理员身份重启」**，确认菜单显示 **「权限: 管理员」**：

   ```
   CaretEngine: 目标进程 pid=<N> 运行在管理员/高权限(UIPI 隔离)，普通权限下可能无法读取其光标
   ```

   每个 pid 只警告一次，重启进程后重新计。
3. **开 `DEBUG` 定位 UIA 卡在哪一层**：将 `log_level=DEBUG` 后重现，查看 `moonlight_sideband.log` 中的
   `[文本光标] UIA 未取到 caret：top=... uia=... ctl=... tp2=... tp=... te=... vp=... ro=... en=... rect=...`，
   可直接看出卡在哪个控件类型、Pattern 或矩形属性上（同一现象只记一次；已拿到 `ControlBox` 兜底时不会打这行）。

### 4.3 与 Android 端（`moonlight-cjcxj`）协同建议
1. **正常命中（`YPercent` ∈ `[0, 10000]`）**：直接按 `YPercent`（以及扩展协议 `CmdID=102` 里的 `ctrl_bottom`、`conf`、`pwd`）让画面避开软键盘。
2. **明确无光标（`YPercent == -1`）**：**不要**把鼠标悬停位置当光标；仅当手机端软键盘正在弹出时，使用“用户最后一次点击屏幕的 Y 坐标”作为键盘避让参考位置。

---

## 五、常见架构问题：为什么输入法知道位置，而不做假输入法能不能同样实现？

### 5.1 为什么系统输入法能拿到坐标，纯外部 `.exe` 却拿不到？
两者走的是 Windows 中**完全不同、且隔离级别不同**的两套接口：
1. **输入法走的是「进程内输入通道」（TSF / IMM32）**
   - 微软拼音 / 搜狗是被 Windows 系统以 DLL 形式**合法加载进每个前台文本进程与 UI 线程内部**的。
   - 无论是 `v2rayN`（Avalonia `TextPresenter`）、`Tai`（WPF `TextStore`）、`WindTerm`（Qt）还是 `Unity`，只要支持中文打字，框架就**必须**在内部实现 TSF `ITfTextStoreACP::GetTextExt` 或调用 `ImmSetCompositionWindow`，主动把精确光标坐标交到同进程里的输入法手上。
2. **外部 `.exe` 走的是「跨进程辅助功能通道」（Win32 / MSAA / UIA）**
   - 出于进程隔离与安全设计，Windows **不提供**任何跨进程读取他人 `HIMC`（`ImmGetCompositionWindow`）或调用目标 UI 线程内 `ITfTextStoreACP` 的公开 API。
   - 而许多非微软原生框对 UIA 的实现并不完整（如 Avalonia 未暴露 `TextPattern2::GetCaretRange`、WPF 空框下 `TextPattern` 无字符矩形、自绘终端/游戏干脆不建 UIA 文本树），因此纯外部进程只能退一步拿 `ControlBox` 外框，或者对完全自绘的画布报 `-1`。

### 5.2 不做「隐藏的假输入法」的替代路线对比
如果不想引入双架构（`x64 + x86`）DLL、管理员安装注册与沙盒 ACL，替代选择如下：

| 路线 | 做法 | 能否取代隐形输入法 | 综合评价 |
|---|---|---|---|
| **路线 1：保留纯绿色单 `.exe`（当前方案）** | Win32 + AppMsg + MSAA + UIA（含父/子穿透与 `ControlBox`）+ 托盘提权 | 能覆盖包括 `v2rayN` / `Tai` 在内的 ~88%–90% 桌面应用；但拿不到 `WindTerm` / `Unity` 的精确行内光标 | ✅ **基础盘必须做好** |
| **路线 2：API Hook DLL 注入** | 写 `x64 + x86` DLL 注入所有进程，Hook `imm32!ImmSetCompositionWindow` | 同样破坏单 EXE、需双架构 DLL；杀软/EDR 极易报毒；现代 TSF 框架不必然每次走 `imm32`，反作弊一样拦 | ❌ **不建议**（不如正规注册 UI-Less 输入法） |
| **路线 3：Android 端触控坐标兜底（强烈推荐配合路线 1）** | PC 抓不到时老实发 `YPercent == -1`；Android 软键盘弹出且收到 `-1` 时，用**用户最后一次点击屏幕的 Y 坐标**避让 | 不追求在游戏/`WindTerm` 里换行追踪，但**100% 达到“软键盘不挡输入位置”的目的**，连锁屏和 Roblox 都能避让 | ✅ **性价比最高** |

**落地取舍结论**：
- **绝大多数情况**：路线 1（当前单 `.exe` 增强）+ 路线 3（Android 端 `-1` 时用最后触控 Y 兜底）即可在不改动 Windows 系统组件、保持纯绿色部署的前提下解决绝大部分防遮挡需求。
- **进阶需求**：只有在必须让 `WindTerm` 或部分支持聚焦期 IME 上报的自绘应用**从 PC 端直接精确上报行内光标（含换行后的精确位置）**时，才需要实施 [隐藏式 Windows 隐形输入法探针方案](HIDDEN_IME_CARET_PROBE.md)。
