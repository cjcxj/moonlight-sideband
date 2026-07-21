# moonlight-sideband

集成式旁路服务后端，与 [Moonlight](https://github.com/moonlight-stream) / [Sunshine](https://github.com/LizardByte/Sunshine) 串流协议完全解耦，通过独立 TCP 通道提供：

1. **光标服务**（兼容原 [windows-cursor-streamer](../windows-cursor-streamer) 协议）
2. **显示器控制**（阶段 2 实现中）
3. **更多功能**（模块化扩展）

## 设计原则

- **不修改底层**：不动 `moonlight-common-c`、不动 `Sunshine` 主线
- **完全向后兼容**：与原 `CursorMonitor.exe` 协议兼容，现有 Android 客户端无需修改
- **模块化**：每个功能以 `ISidebandModule` 实现的形式注册，可独立启停

## 架构

```
┌─────────────────────────────────────────────────────┐
│         moonlight_sideband.exe  (Windows)           │
├─────────────────────────────────────────────────────┤
│  TCP Server (port 5005, 双向)                       │
│  ┌────────────────────────────────────────────────┐ │
│  │  Protocol Router                               │ │
│  │  - 普通包 (Hash != 0xFFFFFFFF) → Cursor Module │ │
│  │  - 控制包 (Hash == 0xFFFFFFFF) → Cmd Dispatcher │ │
│  └────────────────────────────────────────────────┘ │
│  ┌──────────────┐ ┌──────────────┐ ┌────────────┐  │
│  │ CursorModule │ │ DisplayModule│ │ Future...  │  │
│  └──────────────┘ └──────────────┘ └────────────┘  │
└─────────────────────────────────────────────────────┘
              ↑↓ TCP 5005 (双向)
┌─────────────────────────────────────────────────────┐
│  Android (moonlight-cjcxj)                          │
│  - 现有 CursorServiceManager（无需修改即可工作）    │
│  - 未来 SidebandClient（支持双向指令）              │
└─────────────────────────────────────────────────────┘
```

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

| CmdID | 名称 | 方向 | Payload | 状态 |
|---|---|---|---|---|
| 1 | Heartbeat | 双向 | 空 | 计划 |
| 2 | 文本光标状态 | PC→Android | 老格式 | ✅ 已实现 |
| 3 | Hello/握手 | 双向 | JSON | 计划 |
| 10 | 显示器列表请求 | Android→PC | 空 | ✅ |
| 11 | 显示器列表响应 | PC→Android | JSON | ✅ |
| 12 | 切换显示器 | Android→PC | JSON | ✅ |
| 13 | 当前显示器 | PC→Android | JSON | ✅ |
| 14 | 查询显示器模式列表 | Android→PC | JSON | ✅ |
| 15 | 模式列表响应 | PC→Android | JSON | ✅ |
| 16 | 设置分辨率/刷新率 | Android→PC | JSON | ✅ |
| 17 | 设置缩放 | Android→PC | JSON | ✅ |
| 20-24 | Sunshine 配置 | 双向 | JSON | 计划 |
| 100+ | 用户扩展 | 双向 | 任意 | - |

## 模块状态

### CursorModule ✅
- 从 `windows-cursor-streamer` 移植
- 高性能光标捕获（GDI+ + 黑白底差分 + 智能描边）
- 动画光标支持（Sprite Sheet）
- 高 DPI 自适应
- 服务端/客户端双层缓存
- 文本插入符追踪
- 与原协议 100% 兼容

### DisplayModule ✅ 已实现
- `EnumDisplayDevicesW` 枚举所有活动显示器
- `EnumDisplaySettingsExW` 获取分辨率、刷新率、色深
- `GetDpiForMonitor` 获取缩放百分比
- `ChangeDisplaySettingsExW` 切换 Windows 主显示器（立即生效）
- 监控线程每 2 秒检测主显示器变化并主动推送
- 客户端连接时异步推送当前状态
- JSON 响应包含：`id/name/adapter/x/y/w/h/refresh/bpp/scale/is_primary/is_active`

**已知限制**：
- 切换 Windows 主显示器后，Sunshine 捕获的屏幕不会自动跟随
  （需要重启 Sunshine 才能让它捕获新主显示器，这是 Sunshine 的限制）
- 切换主显示器时桌面图标位置会重排（Windows 系统行为）

### 未来模块 💡
- `SunshineModule` - 通过本地 `sunshine.conf` 读写配置
- `ProcessModule` - 进程查询/启动
- `ShortcutModule` - 自定义快捷键脚本

## 编译

### 依赖
- CMake 3.15+
- Visual Studio 2019+ (MSVC) 或 MinGW-w64
- Windows SDK

### 步骤

```powershell
cd D:\SRC\cpp\moonlight-sideband
mkdir build; cd build
cmake ..
cmake --build . --config Release
```

生成 `moonlight_sideband.exe`。

### 命令行参数
```
moonlight_sideband.exe [-l LEVEL] [-p PORT]
  -l LEVEL   日志级别 (TRACE/DEBUG/INFO/ERROR)
  -p PORT    监听端口 (默认 5005)
```

### 日志
程序运行时生成 `moonlight_sideband.log`。

## 与原 windows-cursor-streamer 的关系

| 维度 | windows-cursor-streamer | moonlight-sideband |
|---|---|---|
| 架构 | 单体 main.cpp | 模块化（ISidebandModule） |
| 通信 | 单向 PC→Android | 双向 |
| 协议 | 光标 + 文本光标 | 光标 + 文本光标 + 控制指令 |
| 扩展 | 需改主程序 | 注册新模块即可 |
| 兼容 | - | 完全兼容前者协议 |

原项目保留作为参考实现，新项目是它的超集。

## Android 端集成路线图

1. **阶段 1（已完成）**：现有 `CursorServiceManager` 无需修改即可工作
2. **阶段 2**：扩展 `CursorServiceManager` 增加 `sendCommand()` 双向通信能力
3. **阶段 3**：在 `GameMenu` 中添加"显示器控制"子菜单
4. **阶段 4**：根据需求添加更多功能菜单

## 许可证

继承自 moonlight-cjcxj 项目。
