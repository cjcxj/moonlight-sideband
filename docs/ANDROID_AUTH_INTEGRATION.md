# Android 端接入分级授权 —— 改动方案

面向 `D:\SRC\android\moonlight-cjcxj`。本文只描述**需要改什么**，不含成品代码。

## 背景：为什么必须改

PC 端引入了分级授权。指令按是否改变系统状态分成两类：

| CmdID | 指令 | 需要认证 | 当前 Android 端状态 |
|---|---|---|---|
| 10 | `DISPLAY_LIST_REQ` | 否 | ✅ 继续可用，无需改动 |
| 14 | `DISPLAY_MODE_LIST_REQ` | 否 | ✅ 继续可用，无需改动 |
| 12 | `DISPLAY_SWITCH` | **是** | ❌ 被拒绝 |
| 16 | `DISPLAY_MODE_SET` | **是** | ❌ 被拒绝 |
| 17 | `DISPLAY_SCALE_SET` | **是** | ❌ 被拒绝 |

光标推送、文本光标状态的协议**完全没动**，`CursorServiceManager` 的接收路径不受影响。
也就是说：不改安卓端的话，显示器列表还能拉到，但切换、改分辨率、改缩放三个动作会失败。

PC 端已经做了兼容处理：鉴权失败时用**该请求本该收到的响应 cmd** 回复
（12/16 → 13，17 → 17，10 → 11，14 → 15），payload 为：

```json
{"ok":false,"error":"unauthorized","cmd":12}
```

因此**即使不做任何改动**，`DisplayControlManager.switchDisplay()` 里挂起的请求也能
立即配对上，`onDisplaySwitched(displayId, false, "unauthorized")` 会马上触发，
界面上是一句明确的"unauthorized"而不是等 5 秒的"请求超时"。
但要恢复功能，还是得把下面的认证流程加上。

## 令牌从哪来

PC 端首次运行会在 `moonlight_sideband.exe` 同目录生成 `moonlight_sideband.ini`，
其中 `token=` 一行是 32 位十六进制字符串（128 bit）。托盘右键 →「打开配置文件」
可以直接看到。把这串值粘贴到安卓端设置里即可。

删掉 ini 里的 `token=` 行会在下次启动时重新生成，手机端也要同步更新。

## 需要改的文件

### 1. `sideband/SidebandProtocol.kt` —— 补两个常量

`SidebandCmd` 里加：

```
AUTH_REQ  = 4   // Android -> PC, payload: {"token":"..."}
AUTH_RESP = 5   // PC -> Android, payload: {"ok":true} / {"ok":false,"error":"..."}
```

### 2. 存放令牌 —— 复用现有的 preference 体系

在游戏串流设置里加一个文本输入项（`EditTextPreference` 或你现有的自定义设置项），
键名建议 `sideband_token`，标题「旁路服务令牌」，摘要说明"从 PC 端
moonlight_sideband.ini 复制"。

注意两点：
- 输入框要允许粘贴，且**不要**设成密码类型 —— 用户需要肉眼核对是否粘全了
- 存 `SharedPreferences` 即可。这是局域网内的设备令牌，不是账号密码，
  不必上 EncryptedSharedPreferences；但也别打进日志

### 3. `CursorServiceManager.kt` —— 连接成功后发起认证

在 `startService()` 里 socket 建立成功、接收循环启动**之后**的那个位置，
发一次 `AUTH_REQ`：

- payload 是 `JSONObject().put("token", token).toString().toByteArray()`
- `reqId` 用一个固定值（比如 `Int.MAX_VALUE`）或专门的计数器，避免和
  `DisplayControlManager` 的 `nextReqId` 撞号
- 令牌为空时跳过发送，只记一条日志 —— 这样没配置令牌的用户行为等同于现在

再维护一个 `@Volatile var isAuthenticated: Boolean`：
- 连接建立时置 `false`
- 收到 `AUTH_RESP` 且 `ok=true` 时置 `true`
- **断线重连后必须重置为 false 并重新认证** —— 认证状态是绑在 TCP 连接上的，
  PC 端每条连接独立记录，重连即失效。这是最容易漏的一点

暴露一个查询方法供 UI 用，比如 `fun isSidebandAuthenticated(): Boolean`。

### 4. `sideband/DisplayControlManager.kt` —— 处理认证响应与失败提示

`onCommand()` 的 `when` 里加一个分支处理 `SidebandCmd.AUTH_RESP`。注意它有两种来源：

1. **认证结果本身**：`reqId` 是你发 `AUTH_REQ` 时用的那个
2. **兜底的拒绝响应**：模块没声明固定响应 cmd 时 PC 会用 AUTH_RESP 回
   （目前 DisplayModule 五条指令都声明了，所以实际不会走到，但别假设它不会出现）

另外建议在 `switchDisplay` / `setDisplayMode` / `setDisplayScale` 三个方法里，
发送前先检查 `isSidebandAuthenticated()`，未认证就直接回调失败并提示
「请先在设置中填写旁路服务令牌」，省掉一次无谓的往返。

### 5. `GameMenu.kt` —— 错误提示要能指向解法

现在拿到的 `error` 字符串会是 `"unauthorized"`，直接显示给用户没有意义。
在显示层做一次映射，至少把 `unauthorized` 翻成「未授权：请在设置中填写旁路服务令牌」。

PC 端可能返回的 error 值：

| error | 含义 | 建议提示 |
|---|---|---|
| `unauthorized` | 未认证就发了需认证指令 | 提示去设置里填令牌 |
| `bad_token` | 令牌不匹配 | 提示令牌错误，请重新复制 |
| `server_no_token` | PC 端没配置令牌 | 提示检查 PC 端 ini |
| `not_found` | 目标显示器不存在 | 刷新列表 |
| `already_primary` | 已是主显示器 | 可以忽略或轻提示 |
| `api_failed` | Windows API 调用失败 | 提示查看 PC 端日志 |

## 认证时序

```
Android                              PC
   |-- TCP connect ------------------>|
   |                                  |  连接建立，此连接 authenticated=false
   |-- AUTH_REQ(4) {"token":"..."} -->|
   |<-- AUTH_RESP(5) {"ok":true} -----|  此连接 authenticated=true
   |                                  |
   |-- DISPLAY_LIST_REQ(10) --------->|  只读，本来也不需要认证
   |<-- DISPLAY_LIST_RESP(11) --------|
   |                                  |
   |-- DISPLAY_SWITCH(12) ----------->|  需认证，已通过
   |<-- DISPLAY_CURRENT(13) ----------|
```

未认证时：

```
   |-- DISPLAY_SWITCH(12) ----------->|
   |<-- DISPLAY_CURRENT(13) ----------|  {"ok":false,"error":"unauthorized","cmd":12}
```

## 需要注意的边界

- **认证绑定连接，不绑定设备。** 断线重连、PC 端重启、手机切换网络之后都要重新发
  `AUTH_REQ`。不要把 `isAuthenticated` 做成持久化状态
- **令牌连错 5 次会被断开连接。** PC 端有暴力破解限速，达到上限直接 close socket。
  所以不要写自动重试循环，粘错了应该让用户去改设置
- **认证是否成功不影响光标。** 即使令牌是错的、连接被断开重连，光标推送仍然照常工作，
  用户不会感知到异常 —— 排查时别被"光标是好的所以连接没问题"误导
- **`reqId` 不要用 0。** PC 端主动推送 `DISPLAY_CURRENT` 时 `reqId=0`，
  `DisplayControlManager.onCommand` 正是靠 `reqId > 0` 区分"响应"和"主动推送"的。
  现有 `nextReqId = AtomicInteger(1)` 是对的，加认证时别破坏这个约定

## 验证清单

1. 不填令牌 → 切换显示器应立刻提示未授权（不是等 5 秒超时）
2. 填错令牌 → 提示令牌错误；连错 5 次后连接断开，之后光标应能自动重连恢复
3. 填对令牌 → 切换 / 改分辨率 / 改缩放全部恢复
4. 显示器列表和模式列表在**不填令牌**时也应该正常 —— 这两条本来就不需要认证
5. 认证成功后手动断开 Wi-Fi 再恢复 → 重连后应自动重新认证，功能仍可用
6. 用没改过的旧版 APK 连新版 PC 端 → 切换显示器应立刻报 `unauthorized`，
   而不是超时；光标一切正常
