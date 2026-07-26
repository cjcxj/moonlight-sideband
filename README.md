# moonlight-sideband

集成式旁路服务后端，与 [Moonlight](https://github.com/moonlight-stream) / [Sunshine](https://github.com/LizardByte/Sunshine) 串流协议完全解耦，通过独立 TCP 通道提供：

1. **光标服务**（兼容原 [windows-cursor-streamer](../windows-cursor-streamer) 协议）
2. **显示器控制**（枚举 / 切换 / 分辨率 / 刷新率 / 缩放）
3. **更多功能**（模块化扩展）

## 设计原则

- **不修改底层**：不动 `moonlight-common-c`、不动 `Sunshine` 主线
- **完全向后兼容**：与原 `CursorMonitor.exe` 协议兼容，现有 Android 客户端无需修改
- **模块化**：每个功能以 `ISidebandModule` 实现的形式注册，可独立启停

## 架构

```
┌─────────────────────────────────────────────────────┐
│         moonlight_sideband.exe  (Windows)           │
│         用户会话常驻进程 + 托盘图标                  │
├─────────────────────────────────────────────────────┤
│  TCP Server (port 5005, 双向, WSAPoll 事件驱动)     │
│  ┌────────────────────────────────────────────────┐ │
│  │  Protocol Router                               │ │
│  │  - 普通包 (Hash != 0xFFFFFFFF) → Cursor Module │ │
│  │  - 控制包 (Hash == 0xFFFFFFFF) → 按 CmdID 路由 │ │
│  │    到唯一处理模块，并在此做令牌鉴权            │ │
│  └────────────────────────────────────────────────┘ │
│  ┌──────────────┐ ┌──────────────┐ ┌────────────┐  │
│  │ CursorModule │ │ DisplayModule│ │ Future...  │  │
│  └──────────────┘ └──────────────┘ └────────────┘  │
└─────────────────────────────────────────────────────┘
              ↑↓ TCP 5005 (双向)
┌─────────────────────────────────────────────────────┐
│  Android (moonlight-cjcxj)                          │
│  - 现有 CursorServiceManager（无需修改即可工作）    │
│  - SidebandClient（双向指令 + 令牌认证）            │
└─────────────────────────────────────────────────────┘
```

## 安全模型：分级授权

服务默认监听所有网卡，因此**局域网内任何设备都能连上来**。由于本服务现在能切换显示器、
改分辨率和缩放，不设防是不可接受的。采用的方案是分级授权：

| 操作类型 | 是否需要令牌 | 说明 |
|---|---|---|
| 接收光标推送 | 否 | 老客户端只收不发，完全不受影响 |
| 只读查询（显示器列表、模式列表） | 否 | 不改变系统状态 |
| 切换显示器 / 设分辨率 / 设缩放 | **是** | 需先用 `AUTH_REQ` 提交令牌 |

令牌在首次运行时随机生成（128 bit），写在 exe 同目录的 `moonlight_sideband.ini` 里。
把它填到 Android 端即可。连续 5 次认证失败会断开该连接。

如果你不想用令牌，可以在 ini 里设 `loopback_only=true`，此时只监听本机，
手机需要走 SSH / adb 端口转发。

## 协议

### 普通光标包（向后兼容）
```
[BodyLen(4)] [Hash(4)] [HotX(4)] [HotY(4)] [Frames(4)] [Delay(4)] [PNG...]
Hash != 0xFFFFFFFF
```

### 文本光标状态包（向后兼容，CmdID=2 老格式）
```
[BodyLen=20(4)] [0xFFFFFFFF(4)] [CmdID=2(4)] [YPercent(4)] [0(4)] [0(4)]
```

### 新控制指令包（双向）
```
[BodyLen(4)] [0xFFFFFFFF(4)] [CmdID(4)] [ReqID(4)] [PayloadLen(4)] [Payload...]
```

### 指令命名空间

| CmdID | 名称 | 方向 | Payload | 认证 | 状态 |
|---|---|---|---|---|---|
| 1 | Heartbeat | 双向 | 空 | - | ✅ 原样回包 |
| 2 | 文本光标状态 | PC→Android | 老格式 | - | ✅ |
| 3 | Hello/握手 | 双向 | JSON | - | ✅ 返回能力与认证状态 |
| 4 | 认证请求 | Android→PC | `{"token":"..."}` | - | ✅ |
| 5 | 认证响应 | PC→Android | `{"ok":bool}` | - | ✅ |
| 10 | 显示器列表请求 | Android→PC | 空 | 否 | ✅ |
| 11 | 显示器列表响应 | PC→Android | JSON | - | ✅ |
| 12 | 切换显示器 | Android→PC | JSON | **是** | ✅ |
| 13 | 当前显示器 | PC→Android | JSON | - | ✅ |
| 14 | 查询显示器模式列表 | Android→PC | JSON | 否 | ✅ |
| 15 | 模式列表响应 | PC→Android | JSON | - | ✅ |
| 16 | 设置分辨率/刷新率 | Android→PC | JSON | **是** | ✅ |
| 17 | 设置缩放 | Android→PC | JSON | **是** | ✅ |
| 20-24 | Sunshine 配置 | 双向 | JSON | 是 | 计划 |
| 100+ | 用户扩展 | 双向 | 任意 | - | - |

未通过认证却发送需认证指令时，服务端回一条 `CmdID=5`、
payload 为 `{"ok":false,"error":"unauthorized","cmd":<原指令>}` 的响应（`ReqID` 原样带回）。

## 模块状态

### CursorModule ✅
- 从 `windows-cursor-streamer` 移植
- 高性能光标捕获（GDI+ + 黑白底差分 + 智能描边）
- 动画光标支持（Sprite Sheet）
- 高 DPI 自适应
- 服务端/客户端双层缓存
- 文本插入符追踪（低级钩子只置标志，实际取词在工作线程做，
  避免超时被 Windows 摘掉钩子）
- 与原协议 100% 兼容

### DisplayModule ✅
- CCD `QueryDisplayConfig` 枚举，支持同一 GDI source 下挂多个 target
  （如 `\\.\DISPLAY1#4352` / `#4353`）
- `EnumDisplaySettingsExW` 获取分辨率、刷新率、色深
- CCD `DisplayConfigGetDeviceInfo` 获取/设置缩放，即时生效（移植自 SetDPI）
- `SetDisplayConfig` + 供给配置精确切换到指定 target，切换后轮询验证确已生效
- 显示配置变化由 `WM_DISPLAYCHANGE` 事件驱动推送（另有 10 秒兜底）
- JSON 响应包含：`id/name/adapter/x/y/w/h/refresh/bpp/scale/is_primary/is_active`

**已知限制**：
- 切换 Windows 主显示器后，Sunshine 捕获的屏幕不会自动跟随
  （需要重启 Sunshine 才能让它捕获新主显示器，这是 Sunshine 的限制）
- 切换主显示器时桌面图标位置会重排（Windows 系统行为）

### 未来模块 💡
- `SunshineModule` - 通过本地 `sunshine.conf` 读写配置
- `ProcessModule` - 进程查询/启动
- `ShortcutModule` - 自定义快捷键脚本

## 运行形态

**这是一个跟随用户会话的常驻进程，不是 Windows 服务** —— 这一点是刻意的。

Windows 服务自 Vista 起运行在 Session 0，与交互桌面隔离。跑在 Session 0 里的话：

- `GetCursorInfo` 和全局钩子抓不到用户桌面 → 光标功能失效
- `ChangeDisplaySettingsEx` / `SetDisplayConfig` 作用于 Session 0 的桌面而非用户实际
  在用的桌面 → 显示器控制失效

也就是说做成服务会让两个功能同时报废。要开机自启，请用**托盘右键菜单里的"开机自启"**
（写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，不需要管理员），
或用任务计划程序建一个"登录时触发"的任务。

托盘菜单还提供"打开日志""打开配置文件""退出"。

## 配置

首次运行会在 exe 同目录生成 `moonlight_sideband.ini`：

```ini
port=5005
loopback_only=false
log_level=INFO
log_max_bytes=5242880
token=<首次运行随机生成的 128 bit 令牌>
```

日志写在 exe 同目录的 `moonlight_sideband.log`，超过 `log_max_bytes` 后轮转为 `.log.1`。

日志分级约定（新增日志时照此判断，避免 INFO 又变成垃圾场）：

| 级别 | 放什么 | 例子 |
|---|---|---|
| `TRACE` | 逐帧/逐包细节 | 默认永不开启 |
| `DEBUG` | 诊断细节，频率跟随事件 | 显示器枚举明细、收到的每条指令、各工作线程启停 |
| `INFO` | 低频状态变化 + 用户操作的**结果** | 进程/模块启停、客户端连接、"已切换到 XXX" |
| `WARN` | 异常但已处理，服务仍正常 | API 失败有回退、认证失败、客户端失速被断开 |
| `ERROR` | 功能真的坏了，需要人介入 | 初始化失败、模块启动失败、请求执行失败、崩溃 |

判断 INFO 的标准是：**回头查"刚才发生了什么"时，想看到的就是这几行**。
显示器枚举明细一次切换会打十几行，属于 DEBUG；排查切换问题时把 `log_level`
设成 `DEBUG` 即可看到完整过程。

命令行参数仍可临时覆盖配置（`-p PORT` / `-l LEVEL` / `--loopback`）。
注意程序是 `/SUBSYSTEM:WINDOWS`，**没有控制台**，所以不会有任何 stdout 输出，
诊断信息一律看日志文件。

## 编译

### 依赖
- CMake 3.15+
- **Visual Studio 2019+ (MSVC)** —— 目前只支持 MSVC：
  代码用到 `#pragma comment(lib, ...)`、`localtime_s`、`_strtoui64`、
  以及 `/SUBSYSTEM:WINDOWS` 等 MSVC 专有设施，MinGW-w64 无法直接编译
- Windows SDK（目标 `_WIN32_WINNT=0x0601`，WSAPoll 需要）

### 步骤

```powershell
cd D:\SRC\cpp\moonlight-sideband
cmake -S . -B build
cmake --build build --config Release
```

生成 `build\moonlight_sideband.exe`。

### 单元测试（可选）

只覆盖与平台无关的纯逻辑（JSON 字段解析、协议包构造）——
这两块最容易出静默错误，又完全不需要真机验证。

```powershell
cmake -S . -B build -DSIDEBAND_BUILD_TESTS=ON
cmake --build build --target sideband_tests
ctest --test-dir build
```

## 与原 windows-cursor-streamer 的关系

| 维度 | windows-cursor-streamer | moonlight-sideband |
|---|---|---|
| 架构 | 单体 main.cpp | 模块化（ISidebandModule） |
| 通信 | 单向 PC→Android | 双向 |
| 协议 | 光标 + 文本光标 | 光标 + 文本光标 + 控制指令 |
| 安全 | 无 | 状态变更指令需令牌 |
| 扩展 | 需改主程序 | 注册新模块即可 |
| 兼容 | - | 完全兼容前者协议 |

原项目保留作为参考实现，新项目是它的超集。

## Android 端集成路线图

1. **阶段 1（已完成）**：现有 `CursorServiceManager` 无需修改即可工作
2. **阶段 2**：扩展 `CursorServiceManager` 增加 `sendCommand()` 双向通信能力
3. **阶段 3**：在 `GameMenu` 中添加"显示器控制"子菜单
   —— 需要在发送 CmdID 12/16/17 之前先发一次 `AUTH_REQ`(4) 提交 ini 里的令牌
4. **阶段 4**：根据需求添加更多功能菜单

## 许可证

继承自 moonlight-cjcxj 项目。
