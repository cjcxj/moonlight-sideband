/**
 * DisplayModule 实现
 *
 * 实现：
 * - CCD (QueryDisplayConfig) 枚举显示器路径，支持同一 GDI source 下多个 target
 * - EnumDisplaySettingsExW 获取分辨率/刷新率
 * - CCD DisplayConfigGetDeviceInfo 获取/设置缩放（即时生效）
 * - SetDisplayConfig 供给配置精确切换到指定 target，并轮询验证确已生效
 * - 每 2 秒轮询 + hash 比较监控显示配置变化，仅在状态变化时更新缓存并推送
 */

#include "DisplayModule.hpp"
#include "Logger.hpp"
#include "SidebandProtocol.hpp"
#include "Json.hpp"
#include "Win32Util.hpp"

#include <windows.h>
#include <shellscalingapi.h>

#include <sstream>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <memory>        // unique_ptr：CCD 查询用裸数组缓冲，防止驱动越界写破坏 vector
#include <typeinfo>      // typeid：MonitorLoop 异常诊断输出异常类型名

// 字符串/JSON 辅助已拆到 Win32Util.hpp / Json.hpp。
// 这里做局部引入，避免大面积改动调用点。
using Win32Util::WideToUtf8;

#pragma comment(lib, "shcore.lib")  // GetDpiForMonitor
#pragma comment(lib, "user32.lib")

// ============================================================
//                 MonitorLoop 异常诊断状态（Phase 4）
// ============================================================

// 记录最近一次 CCD 枚举的计数。异常发生时由 MonitorLoop 读出来打日志，
// 不用再靠猜 "vector too long" 到底发生在哪个环节。
// thread_local：MonitorLoop 线程自己写自己读，无需跨线程同步。
static thread_local uint32_t t_lastPathCount = 0;
static thread_local uint32_t t_lastModeCount = 0;
static thread_local size_t t_lastMonitorCount = 0;

// 提取异常类型名。MSVC 的 typeid 输出形如 "class std::length_error"，
// 剥掉修饰前缀得到 "std::length_error"。
static std::string ExceptionTypeName(const std::exception &e)
{
    std::string n = typeid(e).name();
    for (const char *prefix : { "class ", "struct ", "enum " })
    {
        if (n.rfind(prefix, 0) == 0)
        {
            n.erase(0, std::strlen(prefix));
            break;
        }
    }
    return n;
}

// 打印 MonitorLoop 异常诊断块。e 为 nullptr 表示未知异常。
static void LogMonitorLoopException(const std::exception *e)
{
    Logger::Get().Error("DisplayModule MonitorLoop exception:");
    Logger::Get().Error("exception: ", e ? ExceptionTypeName(*e) : "unknown");
    Logger::Get().Error("monitor_count: ", t_lastMonitorCount);
    Logger::Get().Error("path_count: ", t_lastPathCount);
    Logger::Get().Error("mode_count: ", t_lastModeCount);
    Logger::Get().Error("thread: MonitorLoop");
    if (e)
        Logger::Get().Error("what: ", e->what());
}

// ============================================================
//                      辅助函数
// ============================================================

// 从 displayId 解析 GDI 设备名（去掉 #targetId 后缀）
// displayId 格式: "\\.\DISPLAY1#12345" → "\\.\DISPLAY1"
static std::string ParseGdiName(const std::string &displayId)
{
    size_t hashPos = displayId.find('#');
    if (hashPos != std::string::npos)
        return displayId.substr(0, hashPos);
    return displayId;
}

// 从 displayId 解析 targetId
static uint32_t ParseTargetId(const std::string &displayId)
{
    size_t hashPos = displayId.find('#');
    if (hashPos != std::string::npos)
    {
        try { return (uint32_t)std::stoull(displayId.substr(hashPos + 1)); }
        catch (...) { return 0xFFFFFFFF; }
    }
    return 0xFFFFFFFF;
}

// CCD 枚举辅助：按"GetDisplayConfigBufferSizes 取大小 → 分配 → 单次
// QueryDisplayConfig"的顺序查询（规范 2.1），并处理配置变化的 TOCTOU。
// 禁止 static 持久化 vector（跨调用残留垃圾数据）（规范 2.2）。
// 成功后按 API 实际返回的 count 裁剪，避免遍历到值初始化产生的零填充条目
//（DISPLAYCONFIG target id 可能为 0，零填充条目会被误匹配）。
//
// 防御上限：paths<=1024 / modes<=4096，配合 <=0 检查在 vector 创建前挡掉
// "vector too long"（length_error 只能来自 resize/assign 收到超大计数）。
//
// 关键修复（P4）：CCD 存在经典 TOCTOU —— GetDisplayConfigBufferSizes 报 N 后，
// 到真正 QueryDisplayConfig 之间显示配置可能变化，实际路径/模式数超过 N。
// 老代码直接失败返回（枚举结果为空），更糟的是部分驱动会无视缓冲大小把多于
// N 的项写进去，破坏 vector 底层堆 → 后续任意 vector 操作抛 "vector too long"
//（length_error）或访问违规（0xC0000005）崩溃 —— 正是本系统（1 台真实显示器
// 却有 16 条 CCD 路径）启动枚举必崩的根因。
// 这里双重防御：
//   1) 分配时多留余量（paths+16 / modes+64），把轻微"超过报告值"的写入兜进
//      缓冲区，不至于越界破坏堆；
//   2) 收到 ERROR_INSUFFICIENT_BUFFER 后重新取大小再查（最多 3 次），
//      配置真正大幅变化时也能拿到正确结果，而不是放弃。
static bool QueryDisplayConfigData(UINT32 flags,
                                   std::vector<DISPLAYCONFIG_PATH_INFO> &paths,
                                   std::vector<DISPLAYCONFIG_MODE_INFO> &modes)
{
    paths.clear();
    modes.clear();

    constexpr UINT32 kMaxPaths = 1024;
    constexpr UINT32 kMaxModes = 4096;
    constexpr int kMaxRetries = 3;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(flags, &numPaths, &numModes) != ERROR_SUCCESS)
        return false;

    auto countsOk = [&]() {
        return numPaths > 0 && numPaths <= kMaxPaths &&
               numModes > 0 && numModes <= kMaxModes;
    };
    if (!countsOk())
    {
        Logger::Get().Error("DisplayModule: 无效 CCD 计数 paths=", numPaths, " modes=", numModes);
        return false;
    }

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        // 记录本次报告计数，供 MonitorLoop 异常诊断（Phase 4）
        t_lastPathCount = numPaths;
        t_lastModeCount = numModes;

        // 固定按上限分配，不再按"报告值+余量"：切换过程中 CCD 计数会在
        // GetDisplayConfigBufferSizes 与 QueryDisplayConfig 之间剧烈增长
        //（本机 1 台真实显示器却有 16→32 条 CCD 路径），按报告值分配会被
        // 驱动写穿、破坏堆 → 后续任意 vector 操作抛 "vector too long"。
        // 固定 1024/4096 给足 32~64 倍余量，驱动物理上写不穿。
        // 用裸数组而非 vector 接收：极端情况下越界写也只伤我们自己的缓冲区
        // 尾部，不碰 vector 内部结构 / 相邻堆元数据，确认无误后再拷进 vector。
        const UINT32 allocPaths = kMaxPaths;
        const UINT32 allocModes = kMaxModes;
        std::unique_ptr<DISPLAYCONFIG_PATH_INFO[]> pathBuf(new DISPLAYCONFIG_PATH_INFO[allocPaths]);
        std::unique_ptr<DISPLAYCONFIG_MODE_INFO[]> modeBuf(new DISPLAYCONFIG_MODE_INFO[allocModes]);

        UINT32 retPaths = allocPaths, retModes = allocModes;
        LONG r = QueryDisplayConfig(flags, &retPaths, pathBuf.get(),
                                    &retModes, modeBuf.get(), nullptr);
        if (r == ERROR_SUCCESS)
        {
            // 返回计数仍超上限：缓冲区可能已被写穿，防御性拒绝，
            // 绝不把超大计数传进 vector 的 assign/resize。
            if (retPaths > allocPaths || retModes > allocModes)
            {
                Logger::Get().Error("DisplayModule: CCD 返回计数超上限 paths=", retPaths,
                                    " modes=", retModes);
                return false;
            }
            paths.assign(pathBuf.get(), pathBuf.get() + retPaths);
            modes.assign(modeBuf.get(), modeBuf.get() + retModes);
            return true;
        }

        if (r != ERROR_INSUFFICIENT_BUFFER)
            return false;

        // 配置增长：以 QueryDisplayConfig 返回的必要大小为准重试
        Logger::Get().Warning("DisplayModule: CCD 缓冲区不足 flags=", flags,
                              " 报告 paths=", numPaths, " modes=", numModes,
                              " 需 paths=", retPaths, " modes=", retModes, "，重试");
        numPaths = std::max(numPaths, retPaths);
        numModes = std::max(numModes, retModes);
        if (!countsOk())
            return false;
    }

    Logger::Get().Warning("DisplayModule: CCD 查询重试耗尽，放弃 flags=", flags);
    return false;
}

// 计算显示器列表的 FNV-1a 内容 hash，供 MonitorLoop 做变化检测：
// 只要任意显示器属性变化（插拔、几何、分辨率、刷新率、色深、缩放、
// 主显示器切换），结果就不同，从而避免每轮轮询都无条件重写缓存。
static uint64_t HashDisplayState(const std::vector<DisplayModule::DisplayInfo> &displays)
{
    // 先按唯一 id 排序：CCD 返回的路径顺序在两次调用间可能变化，
    // 若直接按原始顺序参与 hash 会导致"状态没变但 hash 变了"的误报。
    auto sorted = displays;
    std::sort(sorted.begin(), sorted.end(),
              [](const DisplayModule::DisplayInfo &a, const DisplayModule::DisplayInfo &b)
              { return a.id < b.id; });

    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    const uint64_t kPrime = 1099511628211ull;

    auto mix = [&h, kPrime](const void *data, size_t len)
    {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= kPrime;
        }
    };
    auto mixInt = [&mix](const int &v) { mix(&v, sizeof(v)); };

    for (const auto &d : sorted)
    {
        mix(d.id.data(), d.id.size());
        mix(d.deviceId.data(), d.deviceId.size());
        mix(d.name.data(), d.name.size());
        mix(d.adapterName.data(), d.adapterName.size());
        mixInt(d.x);
        mixInt(d.y);
        mixInt(d.width);
        mixInt(d.height);
        mixInt(d.refreshRate);
        mixInt(d.bitsPerPel);
        mixInt(d.scale);
        unsigned char flags = (unsigned char)((d.isPrimary ? 1 : 0) | (d.isActive ? 2 : 0));
        mix(&flags, 1);
    }
    return h;
}

// ============================================================
//                      CCD DPI 缩放（移植自 SetDPI）
// ============================================================

// DPI 缩放值表（与 Windows 设置一致）
static const UINT32 kDpiVals[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

// CCD 未公开的 DPI 查询/设置类型
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE =
    (DISPLAYCONFIG_DEVICE_INFO_TYPE)(-3);
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE DISPLAYCONFIG_DEVICE_INFO_SET_DPI_SCALE =
    (DISPLAYCONFIG_DEVICE_INFO_TYPE)(-4);

struct DISPLAYCONFIG_SOURCE_DPI_SCALE_GET
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t minScaleRel;   // 相对于推荐值的最小偏移
    int32_t curScaleRel;   // 相对于推荐值的当前偏移
    int32_t maxScaleRel;   // 相对于推荐值的最大偏移
};

struct DISPLAYCONFIG_SOURCE_DPI_SCALE_SET
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t scaleRel;      // 相对于推荐值的偏移
};

// 通过 GDI 设备名 + targetId 查找 CCD path，返回用于 DPI 查询/设置的 source adapterId + sourceId。
// 私有 DISPLAYCONFIG_DEVICE_INFO_GET/SET_DPI_SCALE 接口的 header.id 填的是 source id
//（参考 lihas/windows-DPI-scaling-sample），填 target id 会导致 SET 返回错误 —— 这是
// 13fc091 把缩放从 sourceId 改成 targetId 后"CCD API 设置缩放失败"的回归根因。
static bool FindSourceByGdiNameAndId(const std::wstring &gdiName, uint32_t targetId,
                                     LUID &outSourceAdapterId, UINT32 &outSourceId)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigData(QDC_ALL_PATHS, paths, modes))
        return false;

    for (const auto &path : paths)
    {
        if (path.targetInfo.id != targetId)
            continue;

        // 先用 GET_SOURCE_NAME 验证 source id 有效（无效则跳过，避免把垃圾 id 传给 DPI 接口）
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = path.sourceInfo.adapterId;
        srcName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(srcName.viewGdiDeviceName, gdiName.c_str()) == 0)
        {
            outSourceAdapterId = path.sourceInfo.adapterId;
            outSourceId = path.sourceInfo.id;
            return true;
        }
    }
    return false;
}

// 获取当前 DPI 缩放百分比
static int GetDpiScalingPercent(LUID adapterId, UINT32 sourceId)
{
    DISPLAYCONFIG_SOURCE_DPI_SCALE_GET req = {};
    req.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE;
    req.header.size = sizeof(req);
    req.header.adapterId = adapterId;
    req.header.id = sourceId;

    if (DisplayConfigGetDeviceInfo(&req.header) != ERROR_SUCCESS)
        return 100;

    // 修正越界值
    if (req.curScaleRel < req.minScaleRel) req.curScaleRel = req.minScaleRel;
    if (req.curScaleRel > req.maxScaleRel) req.curScaleRel = req.maxScaleRel;

    int32_t minAbs = abs((int)req.minScaleRel);
    size_t idx = (size_t)(minAbs + req.curScaleRel);
    if (idx < sizeof(kDpiVals) / sizeof(kDpiVals[0]))
        return (int)kDpiVals[idx];

    return 100;
}

// 设置 DPI 缩放（即时生效）
static bool SetDpiScaling(LUID adapterId, UINT32 sourceId, int dpiPercent)
{
    // 获取当前 DPI 信息
    DISPLAYCONFIG_SOURCE_DPI_SCALE_GET req = {};
    req.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_DPI_SCALE;
    req.header.size = sizeof(req);
    req.header.adapterId = adapterId;
    req.header.id = sourceId;

    LONG getRes = DisplayConfigGetDeviceInfo(&req.header);
    if (getRes != ERROR_SUCCESS)
    {
        Logger::Get().Error("DisplayModule: 查询 DPI 缩放失败 code=", getRes);
        return false;
    }

    // 修正越界值后判断是否已在目标缩放：已到位则直接成功，
    // 避免对部分驱动发出无变化的 SET 而被拒绝。
    if (req.curScaleRel < req.minScaleRel) req.curScaleRel = req.minScaleRel;
    if (req.curScaleRel > req.maxScaleRel) req.curScaleRel = req.maxScaleRel;

    int32_t minAbs = abs((int)req.minScaleRel);
    size_t curIdx = (size_t)(minAbs + req.curScaleRel);
    if (curIdx < sizeof(kDpiVals) / sizeof(kDpiVals[0]) &&
        (int)kDpiVals[curIdx] == dpiPercent)
    {
        return true;
    }

    // 查找目标百分比和推荐百分比在表中的索引
    int idxTarget = -1, idxRecommended = -1;
    for (int i = 0; i < (int)(sizeof(kDpiVals) / sizeof(kDpiVals[0])); ++i)
    {
        if ((int)kDpiVals[i] == dpiPercent) idxTarget = i;
        if (i == minAbs) idxRecommended = i;
    }

    if (idxTarget < 0 || idxRecommended < 0)
        return false;

    int32_t scaleRel = idxTarget - idxRecommended;

    // 目标值必须落在该显示器支持范围内，否则 SET 返回 ERROR_INVALID_PARAMETER
    const size_t tableLen = sizeof(kDpiVals) / sizeof(kDpiVals[0]);
    if (scaleRel < req.minScaleRel || scaleRel > req.maxScaleRel)
    {
        int minScale = 0, maxScale = 0;
        size_t minIdx = (size_t)(minAbs + req.minScaleRel);
        size_t maxIdx = (size_t)(minAbs + req.maxScaleRel);
        if (minIdx < tableLen) minScale = (int)kDpiVals[minIdx];
        if (maxIdx < tableLen) maxScale = (int)kDpiVals[maxIdx];
        Logger::Get().Warning("DisplayModule: 目标缩放 ", dpiPercent,
                              "% 超出支持范围 [", minScale, ", ", maxScale, "]");
        return false;
    }

    DISPLAYCONFIG_SOURCE_DPI_SCALE_SET setReq = {};
    setReq.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_DPI_SCALE;
    setReq.header.size = sizeof(setReq);
    setReq.header.adapterId = adapterId;
    setReq.header.id = sourceId;
    setReq.scaleRel = scaleRel;

    LONG setRes = DisplayConfigSetDeviceInfo(&setReq.header);
    if (setRes != ERROR_SUCCESS)
        Logger::Get().Error("DisplayModule: SET_DPI_SCALE 失败 code=", setRes);
    return setRes == ERROR_SUCCESS;
}

// ============================================================
//                      DisplayModule
// ============================================================

DisplayModule::DisplayModule(SidebandServer &server) : m_server(server) {}

DisplayModule::~DisplayModule()
{
    Stop();
}

bool DisplayModule::Start()
{
    if (m_monitorThread.joinable())
        return true;  // 已启动

    m_exit = false;
    m_monitorThread = std::thread([this]()
                                  { MonitorLoop(); });
    Logger::Get().Info("DisplayModule: 已启动 (含显示器枚举与切换)");
    return true;
}

void DisplayModule::Stop()
{
    m_exit = true;
    {
        std::lock_guard<std::mutex> l(m_wakeMutex);
        m_wakeRequested = true;
    }
    m_wakeCv.notify_all();

    if (m_monitorThread.joinable())
        m_monitorThread.join();

    Logger::Get().Info("DisplayModule: 已停止");
}

bool DisplayModule::HandlesCommand(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    switch (cmd_id)
    {
    case Cmd::DISPLAY_LIST_REQ:
    case Cmd::DISPLAY_SWITCH:
    case Cmd::DISPLAY_MODE_LIST_REQ:
    case Cmd::DISPLAY_MODE_SET:
    case Cmd::DISPLAY_SCALE_SET:
        return true;
    default:
        return false;
    }
}

bool DisplayModule::CommandRequiresAuth(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    // 只读查询（列表 / 模式列表）无需认证，老客户端与只看状态的场景不受影响；
    // 会改变系统状态的三条必须已认证
    switch (cmd_id)
    {
    case Cmd::DISPLAY_SWITCH:
    case Cmd::DISPLAY_MODE_SET:
    case Cmd::DISPLAY_SCALE_SET:
        return true;
    default:
        return false;
    }
}

uint32_t DisplayModule::ResponseCommandFor(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    // 与 OnCommand 里各分支实际用来回包的 cmd 保持一致
    switch (cmd_id)
    {
    case Cmd::DISPLAY_LIST_REQ:      return Cmd::DISPLAY_LIST_RESP;       // 10 -> 11
    case Cmd::DISPLAY_SWITCH:        return Cmd::DISPLAY_CURRENT;         // 12 -> 13
    case Cmd::DISPLAY_MODE_LIST_REQ: return Cmd::DISPLAY_MODE_LIST_RESP;  // 14 -> 15
    case Cmd::DISPLAY_MODE_SET:      return Cmd::DISPLAY_CURRENT;         // 16 -> 13
    case Cmd::DISPLAY_SCALE_SET:     return Cmd::DISPLAY_SCALE_SET;       // 17 -> 17（同 ID 回复）
    default:                         return 0;
    }
}

void DisplayModule::RequestPush()
{
    m_forcePush.store(true);
    {
        std::lock_guard<std::mutex> l(m_wakeMutex);
        m_wakeRequested = true;
    }
    m_wakeCv.notify_one();
}

void DisplayModule::OnDisplayChanged()
{
    // 由主窗口的 WM_DISPLAYCHANGE 在 UI 线程调用 ——
    // 这里只唤醒工作线程，枚举显示器（约 50ms）绝不能放在 UI 线程上做
    Logger::Get().Debug("DisplayModule: 收到 WM_DISPLAYCHANGE");
    RequestPush();
}

void DisplayModule::OnClientConnected(SidebandSession &)
{
    // 不在主循环线程中调用 EnumerateDisplays（会阻塞 ~50ms），
    // 唤醒监控线程去异步推送
    RequestPush();
    Logger::Get().Debug("DisplayModule: 新客户端连接，已请求推送当前显示器状态");
}

// ============================================================
//                      枚举显示器
// ============================================================

std::vector<DisplayModule::DisplayInfo> DisplayModule::EnumerateDisplays() const
{
    // 防止与 ChangeDisplaySettingsExW / SetDisplayConfig 并发导致 CCD 缓冲区溢出
    std::lock_guard<std::mutex> lock(m_displayMutex);

    std::vector<DisplayInfo> result;

    // 用 EnumDisplayMonitors 获取真正在桌面中的显示器列表 + 主显示器
    struct DesktopMonitorsData {
        std::set<std::wstring> activeMonitors;
        std::wstring primaryMonitor;
    } dmData;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM dwData) -> BOOL {
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(hMon, &info))
        {
            auto *data = reinterpret_cast<DesktopMonitorsData*>(dwData);
            data->activeMonitors.insert(info.szDevice);
            if (info.dwFlags & MONITORINFOF_PRIMARY)
                data->primaryMonitor = info.szDevice;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dmData));

    Logger::Get().Debug("DisplayModule: 桌面显示器 count=", dmData.activeMonitors.size(),
                       " 主显示器=", WideToUtf8(dmData.primaryMonitor));
    for (const auto &m : dmData.activeMonitors)
        Logger::Get().Debug("DisplayModule:   桌面: ", WideToUtf8(m));

    // 记录桌面显示器数，供 MonitorLoop 异常诊断（Phase 4）。
    // 在 CCD 查询之前就记：即使后面枚举抛异常，也能看到当时系统里有多少台显示器。
    t_lastMonitorCount = dmData.activeMonitors.size();

    // 获取活跃路径的 target 集合（QDC_ONLY_ACTIVE_PATHS 只返回桌面中的路径）
    auto makeAdapterKey = [](const LUID &luid) -> uint64_t {
        return ((uint64_t)luid.HighPart << 32) | (uint64_t)luid.LowPart;
    };
    std::set<std::pair<uint64_t, uint32_t>> activeTargets;
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> activePaths;
        std::vector<DISPLAYCONFIG_MODE_INFO> activeModes;
        if (QueryDisplayConfigData(QDC_ONLY_ACTIVE_PATHS, activePaths, activeModes))
        {
            for (const auto &ap : activePaths)
            {
                activeTargets.insert({makeAdapterKey(ap.targetInfo.adapterId), ap.targetInfo.id});
            }
        }
    }

    // 用 CCD API 获取所有显示路径（只返回实际存在的物理路径，不含虚拟设备）
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigData(QDC_ALL_PATHS, paths, modes))
        return result;

    // 活跃路径排前面，确保去重时优先保留活跃路径
    std::sort(paths.begin(), paths.end(), [](const DISPLAYCONFIG_PATH_INFO &a, const DISPLAYCONFIG_PATH_INFO &b) {
        bool aActive = (a.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
        bool bActive = (b.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
        return aActive && !bActive;
    });

    // 遍历 CCD 路径，提取显示器信息
    std::set<std::wstring> seenDevices;  // EDID 去重
    for (const auto &path : paths)
    {
        // 获取 source GDI 设备名（如 "\\.\DISPLAY1"）
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        std::wstring gdiName = sourceName.viewGdiDeviceName;
        if (gdiName.empty())
            continue;

        // 过滤非 GDI 显示设备（如断开占位的 "WinDisc"、间接显示器占位设备）
        if (gdiName.rfind(L"\\\\.\\DISPLAY", 0) != 0)
        {
            Logger::Get().Debug("DisplayModule: 跳过非 GDI 设备 ", WideToUtf8(gdiName));
            continue;
        }

        // 获取 target 友好名称和 EDID 信息
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        std::wstring friendlyName;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS)
        {
            friendlyName = targetName.monitorFriendlyDeviceName;
        }

        // 按 target 的 adapterId+id 去重（每个物理接口是唯一的）
        // 同一 GDI 设备名可能有多个 target（如 HDMI1/HDMI2/DP），但它们是不同的物理接口
        uint64_t adapterKey = makeAdapterKey(path.targetInfo.adapterId);
        std::wstring dedupKey = std::to_wstring(adapterKey) + L"_" +
                                std::to_wstring(path.targetInfo.id);

        if (seenDevices.count(dedupKey))
            continue;
        seenDevices.insert(dedupKey);

        DisplayInfo info;
        // id 格式: gdiName#targetId（唯一标识每个 target）
        // 同一 GDI 设备名可能有多个 target（如 HDMI1/HDMI2/DP）
        info.id = WideToUtf8(gdiName) + "#" + std::to_string(path.targetInfo.id);
        info.name = WideToUtf8(friendlyName);
        // isActive 基于 QDC_ONLY_ACTIVE_PATHS（该 target 是否在桌面中）
        info.isActive = activeTargets.count({adapterKey, path.targetInfo.id}) > 0;

        // 用 EnumDisplayDevicesW 获取适配器名称和监视器 DeviceID
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
        {
            if (_wcsicmp(adapter.DeviceName, gdiName.c_str()) == 0)
            {
                info.adapterName = WideToUtf8(adapter.DeviceString);
                break;
            }
        }

        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(gdiName.c_str(), 0, &monitor, 0))
        {
            info.deviceId = WideToUtf8(monitor.DeviceID);
            if (info.name.empty())
                info.name = WideToUtf8(monitor.DeviceString);
        }

        if (info.name.empty())
            info.name = "Display " + std::to_string(result.size() + 1);

        // 获取分辨率/刷新率
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
        {
            info.x = dm.dmPosition.x;
            info.y = dm.dmPosition.y;
            info.width = (int)dm.dmPelsWidth;
            info.height = (int)dm.dmPelsHeight;
            info.refreshRate = (int)dm.dmDisplayFrequency;
            info.bitsPerPel = (int)dm.dmBitsPerPel;
            // isPrimary 基于 MONITORINFOF_PRIMARY 标志（镜像模式下多个显示器位置都是 (0,0)）
            if (info.isActive)
                info.isPrimary = (dmData.primaryMonitor == gdiName);
        }
        else if (EnumDisplaySettingsExW(gdiName.c_str(), ENUM_REGISTRY_SETTINGS, &dm, 0))
        {
            info.x = dm.dmPosition.x;
            info.y = dm.dmPosition.y;
            info.width = (int)dm.dmPelsWidth;
            info.height = (int)dm.dmPelsHeight;
            info.refreshRate = (int)dm.dmDisplayFrequency;
            info.bitsPerPel = (int)dm.dmBitsPerPel;
        }

        // 用 CCD API 获取 DPI 缩放（对活跃和未启用的显示器都有效）。
        // DPI 接口按 source id 寻址；上面的 GET_SOURCE_NAME 已用同一 source id 成功
        // 取到名字，说明该 source 有效，这里再查缩放是安全的（避免把无效 id 传入）。
        info.scale = GetDpiScalingPercent(path.sourceInfo.adapterId, path.sourceInfo.id);

        // 过滤掉未启用且没有真实友好名称的显示器（虚拟设备/无效路径）
        if (!info.isActive && (info.name.empty() || info.name == "Generic PnP Monitor"))
        {
            Logger::Get().Debug("DisplayModule: 跳过虚拟设备 ", info.id, " name=", info.name);
            continue;
        }

        Logger::Get().Debug("DisplayModule: 枚举 ", info.id, " active=", info.isActive,
                           " primary=", info.isPrimary,
                           " ", info.width, "x", info.height, "@", info.refreshRate,
                           " name=", info.name);

        result.push_back(info);
    }

    return result;
}

bool DisplayModule::GetCurrentPrimary(DisplayInfo &out) const
{
    auto displays = EnumerateDisplays();
    for (const auto &d : displays)
    {
        if (d.isPrimary)
        {
            out = d;
            return true;
        }
    }
    return false;
}

// ============================================================
//                      切换主显示器（单显示器模式）
// ============================================================

// 查找 GDI 设备名 + targetId 匹配且未活跃的 target，返回其是否为内部显示器
static bool FindInactiveTargetIsInternal(const std::wstring &gdiName, uint32_t targetId)
{
    auto makeAdapterKey = [](const LUID &luid) -> uint64_t {
        return ((uint64_t)luid.HighPart << 32) | (uint64_t)luid.LowPart;
    };

    // 获取活跃 target 集合
    std::set<std::pair<uint64_t, uint32_t>> activeTargets;
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> ap;
        std::vector<DISPLAYCONFIG_MODE_INFO> am;
        if (QueryDisplayConfigData(QDC_ONLY_ACTIVE_PATHS, ap, am))
        {
            for (const auto &p : ap)
                activeTargets.insert({makeAdapterKey(p.targetInfo.adapterId), p.targetInfo.id});
        }
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigData(QDC_ALL_PATHS, paths, modes))
        return false;

    for (const auto &path : paths)
    {
        // 先匹配 targetId（快速过滤）
        if (path.targetInfo.id != targetId)
            continue;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = path.sourceInfo.adapterId;
        srcName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(srcName.viewGdiDeviceName, gdiName.c_str()) != 0)
            continue;

        uint64_t aKey = makeAdapterKey(path.targetInfo.adapterId);
        bool isActive = activeTargets.count({aKey, path.targetInfo.id}) > 0;
        if (!isActive)
        {
            bool isInternal = (path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL);
            Logger::Get().Debug("DisplayModule: 找到未活跃 target isInternal=", isInternal,
                               " outputTech=", (int)path.targetInfo.outputTechnology);
            return isInternal;
        }
    }
    return false;
}

// 用 CCD 供给配置精确激活指定 target，并停用其他所有 target。
//
// 说明：SDC_TOPOLOGY_INTERNAL/EXTERNAL 只能切换"拓扑类别"，无法指定激活哪个
// target。当两个显示器挂在同一 GDI source（如 \\.\DISPLAY1 下 4352/4353 两个
// target）时，当前拓扑已是 EXTERNAL，再调 SDC_TOPOLOGY_EXTERNAL 是 no-op：
// 返回成功但什么都没变。这里改为提供显式 path 数组：只包含目标 target 的
// path，其余 path 一律不提供（即停用），让 Windows 重算模式。
//
// 优先从显示器数据库（QDC_DATABASE_CURRENT）恢复该 target 上次保存的拓扑与模式：
// Windows 自带切换（Win+P）能保留分辨率/刷新率/缩放，正是因为切换时应用的是
// 数据库里保存的配置，而不是让驱动重选默认模式；这里先尝试同样从数据库取回
// path + mode 提交，失败再回退到"模式索引置无效、由系统自选"的旧逻辑。
static bool ActivateTargetExclusive(const std::wstring &gdiName, uint32_t targetId)
{
    // 方法 A：从数据库恢复已保存配置（与 Win+P 行为一致）
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> dbPaths;
        std::vector<DISPLAYCONFIG_MODE_INFO> dbModes;
        if (QueryDisplayConfigData(QDC_DATABASE_CURRENT, dbPaths, dbModes))
        {
            // 数据库 path 需同时满足：targetId 匹配 + 连接器可用 + source/target
            // 都指向 dbModes 内的合法已保存模式（否则退回让系统自选）
            auto usable = [&](const DISPLAYCONFIG_PATH_INFO &dbp) {
                if (dbp.targetInfo.id != targetId)
                    return false;
                if (!dbp.targetInfo.targetAvailable)
                    return false;
                if (dbp.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                    dbp.targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
                    return false;
                return dbp.sourceInfo.modeInfoIdx < dbModes.size() &&
                       dbp.targetInfo.modeInfoIdx < dbModes.size();
            };

            int exactIdx = -1, looseIdx = -1;
            for (int i = 0; i < (int)dbPaths.size(); ++i)
            {
                if (!usable(dbPaths[i]))
                    continue;

                if (looseIdx < 0)
                    looseIdx = i;

                DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
                srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                srcName.header.size = sizeof(srcName);
                srcName.header.adapterId = dbPaths[i].sourceInfo.adapterId;
                srcName.header.id = dbPaths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
                    continue;

                if (_wcsicmp(srcName.viewGdiDeviceName, gdiName.c_str()) == 0)
                {
                    exactIdx = i;
                    break;
                }
            }

            int dbIdx = (exactIdx >= 0) ? exactIdx : looseIdx;
            if (dbIdx >= 0)
            {
                DISPLAYCONFIG_PATH_INFO p = dbPaths[dbIdx];
                p.flags = DISPLAYCONFIG_PATH_ACTIVE;
                p.sourceInfo.statusFlags = 0;
                p.targetInfo.statusFlags = 0;

                // 只携带本 path 引用的 source/target 两个 mode 并重映射索引，
                // 避免把数据库里其他显示器的 mode 一起提交（部分驱动会拒绝）
                std::vector<DISPLAYCONFIG_MODE_INFO> modeSubset;
                modeSubset.push_back(dbModes[p.sourceInfo.modeInfoIdx]);
                p.sourceInfo.modeInfoIdx = 0;
                modeSubset.push_back(dbModes[p.targetInfo.modeInfoIdx]);
                p.targetInfo.modeInfoIdx = 1;

                LONG r = SetDisplayConfig(1, &p, (UINT32)modeSubset.size(),
                                          modeSubset.data(),
                                          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                          SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
                if (r == ERROR_SUCCESS)
                {
                    Logger::Get().Debug("DisplayModule: ActivateTargetExclusive 从数据库恢复配置 targetId=",
                                        targetId, (exactIdx >= 0 ? " (精确匹配)" : " (宽松匹配)"));
                    return true;
                }
                Logger::Get().Warning("DisplayModule: 从数据库恢复配置 SetDisplayConfig 失败 code=", r,
                                      "，回退模式自选");
            }
        }
    }

    // 方法 B（回退）：QDC_ALL_PATHS 找可用 path，模式索引置无效让系统自选
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigData(QDC_ALL_PATHS, paths, modes))
        return false;

    // 找到目标 path：targetId 匹配 + source GDI 名匹配 + target 可用
    // （找不到 GDI 名匹配时，退而求其次接受仅 targetId 匹配的可用 path，
    //   因为拓扑切换后同一 target 可能被映射到别的 source）
    int exactIdx = -1, looseIdx = -1;
    for (int i = 0; i < (int)paths.size(); ++i)
    {
        if (paths[i].targetInfo.id != targetId)
            continue;
        if (!paths[i].targetInfo.targetAvailable)
            continue;

        if (looseIdx < 0)
            looseIdx = i;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = paths[i].sourceInfo.adapterId;
        srcName.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(srcName.viewGdiDeviceName, gdiName.c_str()) == 0)
        {
            exactIdx = i;
            break;
        }
    }

    int idx = (exactIdx >= 0) ? exactIdx : looseIdx;
    if (idx < 0)
    {
        Logger::Get().Warning("DisplayModule: ActivateTargetExclusive 找不到 targetId=", targetId);
        return false;
    }

    DISPLAYCONFIG_PATH_INFO p = paths[idx];
    p.flags = DISPLAYCONFIG_PATH_ACTIVE;
    // 模式索引置无效，让系统重新选择该显示器的最佳/上次模式
    p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p.sourceInfo.statusFlags = 0;
    p.targetInfo.statusFlags = 0;
    // 未激活 path 上这些字段可能是 0/无效值，给出合法缺省让系统自选
    p.targetInfo.refreshRate.Numerator = 0;
    p.targetInfo.refreshRate.Denominator = 0;
    p.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
    if (p.targetInfo.rotation < DISPLAYCONFIG_ROTATION_IDENTITY ||
        p.targetInfo.rotation > DISPLAYCONFIG_ROTATION_ROTATE270)
        p.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    p.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;

    LONG r = SetDisplayConfig(1, &p, 0, nullptr,
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                              SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (r != ERROR_SUCCESS)
    {
        Logger::Get().Warning("DisplayModule: ActivateTargetExclusive SetDisplayConfig 失败 code=", r,
                            " targetId=", targetId, (exactIdx >= 0 ? " (精确匹配)" : " (宽松匹配)"));
        return false;
    }

    Logger::Get().Debug("DisplayModule: ActivateTargetExclusive 已提交 targetId=", targetId,
                       (exactIdx >= 0 ? " (精确匹配)" : " (宽松匹配)"));
    return true;
}

// 轮询验证指定 target 已激活（拓扑切换是异步生效的）
bool DisplayModule::VerifyTargetActive(uint32_t targetId, int attempts, int intervalMs) const
{
    for (int i = 0; i < attempts; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        auto now = EnumerateDisplays();
        for (const auto &d : now)
        {
            if (ParseTargetId(d.id) == targetId && d.isActive)
                return true;
        }
    }
    return false;
}

DisplayModule::SwitchResult DisplayModule::SwitchPrimaryDisplay(const std::string &displayId)
{
    auto displays = EnumerateDisplays();

    // 找到目标显示器
    const DisplayInfo *target = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId) { target = &d; break; }
    }

    if (!target)
        return SwitchResult::NotFound;

    Logger::Get().Info("DisplayModule: 切换目标 ", displayId,
                       " active=", target->isActive, " primary=", target->isPrimary,
                       " name=", target->name);

    // 只有活跃显示器才能判断 already_primary
    if (target->isActive && target->isPrimary)
        return SwitchResult::AlreadyPrimary;

    std::string gdiNameStr = ParseGdiName(displayId);
    uint32_t targetId = ParseTargetId(displayId);
    std::wstring targetDevName(gdiNameStr.begin(), gdiNameStr.end());

    // 方法 1：CCD 供给配置——显式只激活目标 target（其余全部停用）。
    // 这是唯一能在"同一 source 多个 target"（如 DISPLAY1#4352 / DISPLAY1#4353）
    // 之间来回切换的方法；SDC_TOPOLOGY_* 无法指定 target（见 ActivateTargetExclusive 注释）。
    if (targetId != 0xFFFFFFFF)
    {
        bool activated;
        {
            // 持锁防止与 EnumerateDisplays 并发导致 CCD 缓冲区溢出
            std::lock_guard<std::mutex> dl(m_displayMutex);
            activated = ActivateTargetExclusive(targetDevName, targetId);
        }
        if (activated)
        {
            // 验证真的生效：SetDisplayConfig 可能返回成功但拓扑实际未变，
            // 不验证就会给客户端假的 ok:true
            if (VerifyTargetActive(targetId, 8, 250))
            {
                Logger::Get().Info("DisplayModule: 已切换到 ", displayId, " (CCD 供给配置, 已验证)");
                return SwitchResult::Ok;
            }
            Logger::Get().Warning("DisplayModule: CCD 供给配置提交成功但未生效，尝试回退方案");
        }
    }

    // 方法 2（回退）：目标未启用时用拓扑切换（Win+P 效果，无法指定具体 target）
    if (!target->isActive)
    {
        LONG result;
        bool isInternal;
        {
            // 持锁保护 FindInactiveTargetIsInternal 与 SetDisplayConfig 的 CCD 查询
            std::lock_guard<std::mutex> dl(m_displayMutex);
            isInternal = FindInactiveTargetIsInternal(targetDevName, targetId);
            uint32_t flags = SDC_APPLY | SDC_NO_OPTIMIZATION;
            if (isInternal)
                flags |= SDC_TOPOLOGY_INTERNAL;
            else
                flags |= SDC_TOPOLOGY_EXTERNAL;

            result = SetDisplayConfig(0, nullptr, 0, nullptr, flags);
        }
        if (result == ERROR_SUCCESS && VerifyTargetActive(targetId, 8, 250))
        {
            Logger::Get().Info("DisplayModule: SetDisplayConfig 拓扑切换到",
                                (isInternal ? "内部" : "外部"), "显示器 ", displayId, " (已验证)");
            return SwitchResult::Ok;
        }

        Logger::Get().Warning("DisplayModule: 拓扑切换失败或未生效 code=", result,
                            "，尝试 ChangeDisplaySettingsExW");
        // 继续尝试 ChangeDisplaySettingsExW
    }

    // 目标已启用：用 ChangeDisplaySettingsExW 切换主显示器
    Logger::Get().Debug("DisplayModule: 切换主显示器 ", displayId);

    {
        // 持锁防止与 EnumerateDisplays 并发
        std::lock_guard<std::mutex> dl(m_displayMutex);

        // 1. 启用目标显示器，设为 (0,0) + CDS_SET_PRIMARY
        DEVMODEW targetDm = {};
        targetDm.dmSize = sizeof(targetDm);
        targetDm.dmDriverExtra = 0;

        if (!EnumDisplaySettingsExW(targetDevName.c_str(), ENUM_CURRENT_SETTINGS, &targetDm, 0))
        {
            if (!EnumDisplaySettingsExW(targetDevName.c_str(), ENUM_REGISTRY_SETTINGS, &targetDm, 0))
            {
                Logger::Get().Error("DisplayModule: 获取目标显示器模式失败");
                return SwitchResult::ApiFailed;
            }
        }

        targetDm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        targetDm.dmPosition.x = 0;
        targetDm.dmPosition.y = 0;

        LONG r1 = ChangeDisplaySettingsExW(targetDevName.c_str(), &targetDm, nullptr,
                                           CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET,
                                           nullptr);
        if (r1 != DISP_CHANGE_SUCCESSFUL)
        {
            Logger::Get().Error("DisplayModule: 设置主显示器失败, code=", r1);
            return SwitchResult::ApiFailed;
        }

        // 2. 禁用其他所有显示器
        // 注意：d.id 形如 "\\.\DISPLAY2#4353"，必须先去掉 #targetId 后缀才是
        // ChangeDisplaySettingsExW 能识别的 GDI 设备名；同一 GDI source 只处理一次
        std::set<std::string> handledGdi;
        for (const auto &d : displays)
        {
            std::string otherGdi = ParseGdiName(d.id);
            if (_stricmp(otherGdi.c_str(), gdiNameStr.c_str()) == 0)
                continue;  // 目标所在的 source 不能禁
            if (!handledGdi.insert(otherGdi).second)
                continue;  // 该 source 已处理过
            if (!d.isActive)
                continue;  // 本来就没启用

            std::wstring devName(otherGdi.begin(), otherGdi.end());
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            dm.dmDriverExtra = 0;

            if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
                EnumDisplaySettingsExW(devName.c_str(), ENUM_REGISTRY_SETTINGS, &dm, 0);

            dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
            dm.dmPelsWidth = 0;
            dm.dmPelsHeight = 0;
            dm.dmPosition.x = 0;
            dm.dmPosition.y = 0;

            LONG rd = ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr,
                                               CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            if (rd != DISP_CHANGE_SUCCESSFUL)
                Logger::Get().Warning("DisplayModule: 禁用 ", otherGdi, " 失败 code=", rd);
        }

        // 3. 应用所有更改
        LONG r2 = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (r2 != DISP_CHANGE_SUCCESSFUL)
        {
            Logger::Get().Error("DisplayModule: 应用更改失败, code=", r2);
            return SwitchResult::ApiFailed;
        }
    }

    // 最终验证：确保目标 target 真的处于激活状态，绝不给客户端假的 ok:true
    if (targetId != 0xFFFFFFFF && !VerifyTargetActive(targetId, 4, 250))
    {
        Logger::Get().Error("DisplayModule: 切换流程走完但目标 target 未激活 ", displayId);
        return SwitchResult::ApiFailed;
    }

    Logger::Get().Info("DisplayModule: 已切换到 ", displayId, " (其他显示器已禁用)");

    return SwitchResult::Ok;
}

// ============================================================
//                      枚举显示模式
// ============================================================

std::vector<DisplayModule::DisplayMode> DisplayModule::EnumerateModes(const std::string &displayId) const
{
    std::lock_guard<std::mutex> lock(m_displayMutex);

    std::vector<DisplayMode> result;
    std::string gdiNameStr = ParseGdiName(displayId);
    std::wstring devName(gdiNameStr.begin(), gdiNameStr.end());

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;

    DWORD i = 0;
    while (EnumDisplaySettingsExW(devName.c_str(), i, &dm, 0))
    {
        // 仅保留 32 位色深（低于 24 的过滤掉，避免列表过长）
        if (dm.dmBitsPerPel >= 24)
        {
            DisplayMode m;
            m.width = (int)dm.dmPelsWidth;
            m.height = (int)dm.dmPelsHeight;
            m.refreshRate = (int)dm.dmDisplayFrequency;
            m.bitsPerPel = (int)dm.dmBitsPerPel;

            // 去重（同 w/h/refresh/bpp）
            bool dup = false;
            for (const auto &e : result)
            {
                if (e.width == m.width && e.height == m.height &&
                    e.refreshRate == m.refreshRate && e.bitsPerPel == m.bitsPerPel)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                result.push_back(m);
        }
        ++i;

        // 限制枚举数量避免异常驱动导致死循环
        if (i > 1024)
            break;
    }

    // 排序：分辨率降序 → 刷新率降序
    std::sort(result.begin(), result.end(), [](const DisplayMode &a, const DisplayMode &b) {
        if (a.width != b.width) return a.width > b.width;
        if (a.height != b.height) return a.height > b.height;
        return a.refreshRate > b.refreshRate;
    });

    return result;
}

// ============================================================
//                      设置分辨率/刷新率
// ============================================================

DisplayModule::SetModeResult DisplayModule::SetDisplayMode(const std::string &displayId, int w, int h, int refresh)
{
    auto displays = EnumerateDisplays();
    const DisplayInfo *target = nullptr;
    for (const auto &d : displays)
    {
        if (d.id == displayId) { target = &d; break; }
    }
    if (!target) return SetModeResult::NotFound;
    if (!target->isActive) return SetModeResult::NotActive;

    // 验证模式是否在支持列表中
    auto modes = EnumerateModes(displayId);
    bool found = false;
    for (const auto &m : modes)
    {
        if (m.width == w && m.height == h && m.refreshRate == refresh)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        Logger::Get().Warning("DisplayModule: 模式不支持 ", w, "x", h, "@", refresh);
        return SetModeResult::ModeNotFound;
    }

    std::string gdiNameStr = ParseGdiName(displayId);
    std::wstring devName(gdiNameStr.begin(), gdiNameStr.end());
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
    {
        Logger::Get().Error("DisplayModule: 获取当前模式失败");
        return SetModeResult::ApiFailed;
    }

    dm.dmPelsWidth = (DWORD)w;
    dm.dmPelsHeight = (DWORD)h;
    dm.dmDisplayFrequency = (DWORD)refresh;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    // 持有互斥锁以防止 CCD 查询（EnumerateDisplays）在模式变更期间并发执行，
    // 避免 GetDisplayConfigBufferSizes / QueryDisplayConfig 之间的 TOCTOU 竞争。
    // EnumerateDisplays / EnumerateModes 已在函数开头各自获取了同一把锁，
    // 但在它们返回后锁已释放，这里重新获取不影响。
    {
        std::lock_guard<std::mutex> lock(m_displayMutex);

        LONG r = ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr,
                                           CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL)
        {
            Logger::Get().Error("DisplayModule: 设置模式失败 code=", r);
            return SetModeResult::ApiFailed;
        }

        // 应用
        r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL)
        {
            Logger::Get().Error("DisplayModule: 应用模式失败 code=", r);
            return SetModeResult::ApiFailed;
        }
    }

    Logger::Get().Info("DisplayModule: 已设置 ", displayId, " -> ", w, "x", h, "@", refresh);
    return SetModeResult::Ok;
}

// ============================================================
//                      设置缩放（CCD API 即时生效）
// ============================================================

// 校验缩放值是否在支持列表中
static bool IsValidDpiScale(int scale)
{
    for (auto v : kDpiVals)
    {
        if ((int)v == scale) return true;
    }
    return false;
}

DisplayModule::SetScaleResult DisplayModule::SetDisplayScale(const std::string &displayId, int scale, bool *immediate)
{
    if (!IsValidDpiScale(scale))
        return SetScaleResult::InvalidScale;

    // 未启用的显示器不允许改缩放。
    // 之前没有这道检查：CCD target 查找会成功（QDC_ALL_PATHS 里也包含未激活路径），
    // 但随后的私有 DPI 接口对不在桌面中的 target 会失败，于是返回 registry_failed ——
    // 一个和真实原因毫无关系的错误码。这里提前判定，与 SetDisplayMode 保持一致。
    {
        auto displays = EnumerateDisplays();
        const DisplayInfo *target = nullptr;
        for (const auto &d : displays)
        {
            if (d.id == displayId) { target = &d; break; }
        }
        if (!target)
            return SetScaleResult::NotFound;
        if (!target->isActive)
        {
            Logger::Get().Warning("DisplayModule: 显示器未启用，拒绝设置缩放 ", displayId);
            return SetScaleResult::NotActive;
        }
    }

    // 通过 GDI 设备名 + targetId 查找 CCD path 的 source（DPI 接口需要 source id）
    std::string gdiNameStr = ParseGdiName(displayId);
    uint32_t targetId = ParseTargetId(displayId);
    std::wstring devName(gdiNameStr.begin(), gdiNameStr.end());
    LUID sourceAdapterId = {};
    UINT32 sourceId = 0;

    // 持锁防止与 EnumerateDisplays 并发导致 CCD 查询结果不一致
    {
        std::lock_guard<std::mutex> dl(m_displayMutex);
        if (targetId == 0xFFFFFFFF ||
            !FindSourceByGdiNameAndId(devName, targetId, sourceAdapterId, sourceId))
        {
            Logger::Get().Error("DisplayModule: 找不到显示器 ", displayId, " 的 CCD source");
            return SetScaleResult::NotFound;
        }

        if (SetDpiScaling(sourceAdapterId, sourceId, scale))
        {
            if (immediate) *immediate = true;
            Logger::Get().Info("DisplayModule: 已设置缩放 ", displayId, " -> ", scale,
                               "% (CCD API, 即时生效)");
            return SetScaleResult::Ok;
        }
    }

    Logger::Get().Error("DisplayModule: CCD API 设置缩放失败 ", displayId, " -> ", scale, "%");
    if (immediate) *immediate = false;
    return SetScaleResult::RegistryFailed;
}

// ============================================================
//                      JSON 序列化
// ============================================================

std::string DisplayModule::DisplayToJson(const DisplayInfo &d) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\":\"" << Json::Escape(d.id) << "\",";
    ss << "\"device_id\":\"" << Json::Escape(d.deviceId) << "\",";
    ss << "\"name\":\"" << Json::Escape(d.name) << "\",";
    ss << "\"adapter\":\"" << Json::Escape(d.adapterName) << "\",";
    ss << "\"x\":" << d.x << ",";
    ss << "\"y\":" << d.y << ",";
    ss << "\"w\":" << d.width << ",";
    ss << "\"h\":" << d.height << ",";
    ss << "\"refresh\":" << d.refreshRate << ",";
    ss << "\"bpp\":" << d.bitsPerPel << ",";
    ss << "\"scale\":" << d.scale << ",";
    ss << "\"is_primary\":" << (d.isPrimary ? "true" : "false") << ",";
    ss << "\"is_active\":" << (d.isActive ? "true" : "false");
    ss << "}";
    return ss.str();
}

std::string DisplayModule::DisplaysToJson(const std::vector<DisplayInfo> &displays) const
{
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < displays.size(); ++i)
    {
        if (i > 0)
            ss << ",";
        ss << DisplayToJson(displays[i]);
    }
    ss << "]";
    return ss.str();
}

std::string DisplayModule::ModesToJson(const std::string &displayId,
                                        const std::vector<DisplayMode> &modes) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"display_id\":\"" << Json::Escape(displayId) << "\",";
    ss << "\"modes\":[";
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (i > 0) ss << ",";
        const auto &m = modes[i];
        ss << "{\"w\":" << m.width;
        ss << ",\"h\":" << m.height;
        ss << ",\"refresh\":" << m.refreshRate;
        ss << ",\"bpp\":" << m.bitsPerPel << "}";
    }
    ss << "]}";
    return ss.str();
}

// ============================================================
//                      指令处理
// ============================================================

void DisplayModule::OnCommand(SidebandSession &session,
                              uint32_t cmd_id,
                              uint32_t req_id,
                              const uint8_t *payload,
                              uint32_t payload_len)
{
    using namespace SidebandProtocol;

    switch (cmd_id)
    {
    case Cmd::DISPLAY_LIST_REQ:
    {
        auto displays = EnumerateDisplays();
        std::string json = DisplaysToJson(displays);
        std::vector<uint8_t> p(json.begin(), json.end());
        session.SendCommand(Cmd::DISPLAY_LIST_RESP, req_id, p);
        Logger::Get().Debug("DisplayModule: 列出显示器 (count=", displays.size(), ")");
        break;
    }
    case Cmd::DISPLAY_SWITCH:
    {
        // payload 期望是 JSON: {"display_id":"\\\\.\\DISPLAY2"}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        Logger::Get().Debug("DisplayModule: DISPLAY_SWITCH payload=", payloadStr);

        std::string displayId = Json::GetString(payloadStr, "display_id");

        std::string respJson;
        if (displayId.empty())
        {
            respJson = R"({"ok":false,"error":"invalid_payload","msg":"missing display_id"})";
        }
        else
        {
            SwitchResult r = SwitchPrimaryDisplay(displayId);
            switch (r)
            {
            case SwitchResult::Ok:
                respJson = R"({"ok":true,"display_id":")" + Json::Escape(displayId) + R"("})";
                break;
            case SwitchResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found","display_id":")" + Json::Escape(displayId) + R"("})";
                break;
            case SwitchResult::AlreadyPrimary:
                respJson = R"({"ok":false,"error":"already_primary","display_id":")" + Json::Escape(displayId) + R"("})";
                break;
            case SwitchResult::NotActive:
                respJson = R"({"ok":false,"error":"not_active","display_id":")" + Json::Escape(displayId) + R"("})";
                break;
            case SwitchResult::ApiFailed:
                respJson = R"({"ok":false,"error":"api_failed","display_id":")" + Json::Escape(displayId) + R"("})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_CURRENT, req_id, p);

        // 切换成功后唤醒监控线程异步广播新状态。
        // （不在这里直接 BroadcastCommand：切换是耗时操作，且状态需要重新枚举后才准确）
        if (respJson.find("\"ok\":true") != std::string::npos)
        {
            RequestPush();
        }
        break;
    }
    case Cmd::DISPLAY_MODE_LIST_REQ:
    {
        // payload: JSON: {"display_id":"\\\\.\\DISPLAY1"}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = Json::GetString(payloadStr, "display_id");

        std::string respJson;
        if (displayId.empty())
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            auto modes = EnumerateModes(displayId);
            respJson = "{\"ok\":true," + ModesToJson(displayId, modes).substr(1);
        }
        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_MODE_LIST_RESP, req_id, p);
        Logger::Get().Debug("DisplayModule: 查询模式列表 ", displayId);
        break;
    }
    case Cmd::DISPLAY_MODE_SET:
    {
        // payload: JSON: {"display_id":"...","w":1920,"h":1080,"refresh":60}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = Json::GetString(payloadStr, "display_id");
        int w = Json::GetInt(payloadStr, "w");
        int h = Json::GetInt(payloadStr, "h");
        int refresh = Json::GetInt(payloadStr, "refresh");

        std::string respJson;
        if (displayId.empty() || w <= 0 || h <= 0 || refresh <= 0)
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            SetModeResult r = SetDisplayMode(displayId, w, h, refresh);
            switch (r)
            {
            case SetModeResult::Ok:
                respJson = "{\"ok\":true,\"display_id\":\"" + Json::Escape(displayId) +
                           "\",\"w\":" + std::to_string(w) +
                           ",\"h\":" + std::to_string(h) +
                           ",\"refresh\":" + std::to_string(refresh) + "}";
                break;
            case SetModeResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found"})";
                break;
            case SetModeResult::NotActive:
                respJson = R"({"ok":false,"error":"not_active"})";
                break;
            case SetModeResult::ModeNotFound:
                respJson = R"({"ok":false,"error":"mode_not_found"})";
                break;
            case SetModeResult::ApiFailed:
                respJson = R"({"ok":false,"error":"api_failed"})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        session.SendCommand(Cmd::DISPLAY_CURRENT, req_id, p);

        // 分辨率变化后异步推送新状态
        if (respJson.find("\"ok\":true") != std::string::npos)
        {
            RequestPush();
        }
        break;
    }
    case Cmd::DISPLAY_SCALE_SET:
    {
        // payload: JSON: {"display_id":"...","scale":125}
        std::string payloadStr(payload ? (const char *)payload : "",
                               payload ? payload_len : 0);
        std::string displayId = Json::GetString(payloadStr, "display_id");
        int scale = Json::GetInt(payloadStr, "scale");

        std::string respJson;
        if (displayId.empty() || scale <= 0)
        {
            respJson = R"({"ok":false,"error":"invalid_payload"})";
        }
        else
        {
            bool immediate = false;
            SetScaleResult r = SetDisplayScale(displayId, scale, &immediate);
            switch (r)
            {
            case SetScaleResult::Ok:
                respJson = "{\"ok\":true,\"display_id\":\"" + Json::Escape(displayId) +
                           "\",\"scale\":" + std::to_string(scale) +
                           ",\"requires_sign_out\":" + (immediate ? "false" : "true") + "}";
                break;
            case SetScaleResult::NotFound:
                respJson = R"({"ok":false,"error":"not_found"})";
                break;
            case SetScaleResult::NotActive:
                respJson = R"({"ok":false,"error":"not_active"})";
                break;
            case SetScaleResult::InvalidScale:
                respJson = R"({"ok":false,"error":"invalid_scale","msg":"allowed: 100,125,150,175,200,225,250,300,350,400,450,500"})";
                break;
            case SetScaleResult::RegistryFailed:
                respJson = R"({"ok":false,"error":"registry_failed"})";
                break;
            }
        }

        std::vector<uint8_t> p(respJson.begin(), respJson.end());
        // 缩放不立即生效，用 DISPLAY_SCALE_SET 作为响应（客户端可识别）
        session.SendCommand(Cmd::DISPLAY_SCALE_SET, req_id, p);
        break;
    }
    default:
        Logger::Get().Debug("DisplayModule: 未处理的 cmd_id=", cmd_id);
        break;
    }
}

// ============================================================
//                      监控循环
// ============================================================

void DisplayModule::MonitorLoop()
{
    Logger::Get().Debug("DisplayModule: 监控线程已启动");

    // 上次状态缓存：hash 不变则跳过更新/推送。异常时重置为"全新状态"。
    DisplayState last;

    // 上一轮是否失败：决定下一轮休眠时长（失败 5 秒，正常 2 秒）。
    bool lastRoundFailed = false;

    // 异常日志节流（P0）：同一个错误最多每 30 秒刷一条诊断，
    // 避免连续异常时每 2 秒刷屏 "vector too long" 浪费 CPU 写日志。
    auto lastErrorLog = std::chrono::steady_clock::now() - std::chrono::seconds(30);

    while (!m_exit)
    {
        try
        {
            // 强制推送：客户端连接 / 切换 / 分辨率 / 缩放完成后置位，
            // 不依赖变化检测，确保第一时间把最新状态推给客户端
            bool force = m_forcePush.exchange(false);

            auto current = EnumerateDisplays();
            uint64_t hash = HashDisplayState(current);

            // hash 比较：没有变化就不做任何更新（"不要每次都写"）
            if (force || hash != last.hash)
            {
                last.hash = hash;
                last.displays = std::move(current);

                if (force || m_server.HasClients())
                    PushCurrentDisplayState(0);
            }

            // 一整轮枚举 + 推送成功：恢复正常轮询节奏
            lastRoundFailed = false;
        }
        catch (const std::exception &e)
        {
            lastRoundFailed = true;
            last = DisplayState{};
            auto now = std::chrono::steady_clock::now();
            if (now - lastErrorLog >= std::chrono::seconds(30))
            {
                lastErrorLog = now;
                LogMonitorLoopException(&e);
            }
        }
        catch (...)
        {
            lastRoundFailed = true;
            last = DisplayState{};
            auto now = std::chrono::steady_clock::now();
            if (now - lastErrorLog >= std::chrono::seconds(30))
            {
                lastErrorLog = now;
                LogMonitorLoopException(nullptr);
            }
        }

        // 注意：异常后只标记状态无效并休眠，绝不做"重启监控"。
        // 重启会立刻再次触发同一个错误（异常循环），还要重建所有本地状态；
        // 这里原地重试，靠 5 秒休眠放慢节奏，等显示器驱动 / CCD 自己稳定。
        // 正常轮询 2 秒；异常后改为 5 秒（P3）。
        // WM_DISPLAYCHANGE / 客户端连接 / 命令完成 / Stop 可提前唤醒。
        const auto pollInterval =
            lastRoundFailed ? std::chrono::seconds(5) : std::chrono::seconds(2);

        std::unique_lock<std::mutex> l(m_wakeMutex);
        m_wakeCv.wait_for(l, pollInterval,
                          [this] { return m_wakeRequested || m_exit; });
        m_wakeRequested = false;
    }

    Logger::Get().Debug("DisplayModule: 监控线程退出");
}

void DisplayModule::PushCurrentDisplayState(uint32_t req_id)
{
    DisplayInfo primary;
    if (!GetCurrentPrimary(primary))
    {
        std::string err = R"({"ok":false,"error":"no_primary"})";
        std::vector<uint8_t> p(err.begin(), err.end());
        m_server.BroadcastCommand(SidebandProtocol::Cmd::DISPLAY_CURRENT, req_id, p);
        return;
    }

    // 单条推送格式（与 DISPLAY_SWITCH 的响应一致）
    std::ostringstream ss;
    ss << "{";
    ss << "\"ok\":true,";
    ss << "\"display_id\":\"" << Json::Escape(primary.id) << "\",";
    ss << "\"name\":\"" << Json::Escape(primary.name) << "\",";
    ss << "\"w\":" << primary.width << ",";
    ss << "\"h\":" << primary.height << ",";
    ss << "\"refresh\":" << primary.refreshRate << ",";
    ss << "\"scale\":" << primary.scale;
    ss << "}";

    std::vector<uint8_t> p(ss.str().begin(), ss.str().end());
    m_server.BroadcastCommand(SidebandProtocol::Cmd::DISPLAY_CURRENT, req_id, p);

    Logger::Get().Debug("DisplayModule: 推送主显示器 ", primary.id,
                        " ", primary.width, "x", primary.height,
                        "@", primary.refreshRate, "Hz scale=", primary.scale, "%");
}
