/**
 * CursorModule 实现
 *
 * 移植自 D:\SRC\cpp\windows-cursor-streamer\main.cpp
 * 主要变更：
 * - 拆分 CursorEngine 与 CursorModule
 * - CursorEngine 通过 SidebandServer 广播（不再直接操作 socket）
 * - 加入模块生命周期管理（Start/Stop）
 * - 钩子回调使用静态单例转发
 */

#include "CursorModule.hpp"
#include "Logger.hpp"
#include "SidebandProtocol.hpp"

#include <windows.h>
#include <shellscalingapi.h>
#include <gdiplus.h>
// UIA客户端接口。放在 windows.h 之后（先 winsock2 顺序由 SidebandSession.hpp
// 的包含顺序保证 —— 本文件经由 CursorModule.hpp 间接包含 winsock2.h）。
#include <uiautomation.h>
#include <ole2.h>
#include <oleauto.h>   // SafeArrayAccessData / SafeArrayGetElement

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
// UIA（IUIAutomation 来自 uiautomationclient.lib；uiautomation.h 里自带
// #pragma comment(lib, ...) 的 MSVC 用户不存在此问题，这里显式补上保险）
#pragma comment(lib, "uiautomationcore.lib")

// 全局单例（钩子回调转发用）
CursorModule *CursorModule::s_instance = nullptr;

// ============================================================
//                      CursorEngine
// ============================================================

static uint32_t CalculateCRC32(const std::vector<uint8_t> &data)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data)
    {
        crc ^= byte;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return ~crc;
}

int CursorEngine::GetEncoderClsid(const WCHAR *format, CLSID *pClsid)
{
    UINT num, size;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;
    std::vector<char> buf(size);
    Gdiplus::ImageCodecInfo *p = (Gdiplus::ImageCodecInfo *)buf.data();
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(p[j].MimeType, format) == 0)
        {
            *pClsid = p[j].Clsid;
            return j;
        }
    }
    return -1;
}

bool CursorEngine::RecreateResources(int w, int hTotal)
{
    if (m_hMemDC && m_hBmpB && m_hBmpW && w == m_cachedWidth && hTotal == m_cachedHeight)
        return true;

    FreeResources();

    HDC hScreen = GetDC(NULL);
    m_hMemDC = CreateCompatibleDC(hScreen);
    ReleaseDC(NULL, hScreen);
    if (!m_hMemDC)
        return false;

    BITMAPINFOHEADER bi = {sizeof(bi)};
    bi.biWidth = w;
    bi.biHeight = -hTotal; // Top-Down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    m_hBmpB = CreateDIBSection(m_hMemDC, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &m_pBitsB, NULL, 0);
    m_hBmpW = CreateDIBSection(m_hMemDC, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &m_pBitsW, NULL, 0);
    if (!m_hBmpB || !m_hBmpW)
    {
        FreeResources();
        return false;
    }
    m_cachedWidth = w;
    m_cachedHeight = hTotal;
    return true;
}

void CursorEngine::FreeResources()
{
    if (m_hMemDC)
        DeleteDC(m_hMemDC);
    if (m_hBmpB)
        DeleteObject(m_hBmpB);
    if (m_hBmpW)
        DeleteObject(m_hBmpW);
    m_hMemDC = NULL;
    m_hBmpB = NULL;
    m_hBmpW = NULL;
    m_pBitsB = NULL;
    m_pBitsW = NULL;
    m_cachedWidth = 0;
    m_cachedHeight = 0;
}

int CursorEngine::GetTargetSize()
{
    static int s_size = 32;
    // epoch 初始化：首次调用立即读注册表拿真实值，而不是先返回默认 32、
    // 2s 后才刷新（否则启动时会触发一次虚假的"32 -> 实际值"重置）
    static auto s_lastCheck = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastCheck).count() > 2)
    {
        s_lastCheck = now;
        HKEY k;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Control Panel\\Cursors", 0, KEY_READ, &k) == 0)
        {
            DWORD t, sz = 4, v = 0;
            if (RegQueryValueExA(k, "CursorBaseSize", 0, &t, (BYTE *)&v, &sz) == 0)
                s_size = v;
            RegCloseKey(k);
        }
    }
    return std::clamp(s_size, 32, 256);
}

UINT CursorEngine::GetCursorMonitorDPI()
{
    POINT pt;
    if (GetCursorPos(&pt))
    {
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        auto now = std::chrono::steady_clock::now();
        bool isCacheStale = (hMon != mLastMonitor) ||
                            (std::chrono::duration_cast<std::chrono::seconds>(now - mLastDpiCheckTime).count() >= 2);
        if (isCacheStale)
        {
            UINT dpiX, dpiY;
            if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            {
                mLastDpi = dpiX;
                mLastMonitor = hMon;
                mLastDpiCheckTime = now;
            }
        }
    }
    return mLastDpi;
}

int CursorEngine::GetExpectedSystemCursorSize(int baseSize, UINT dpi)
{
    int calculated = MulDiv(baseSize, dpi, 96);
    if (calculated >= 96)
        return 96;
    if (calculated >= 64)
        return 64;
    if (calculated >= 48)
        return 48;
    return 32;
}

std::pair<int, int> CursorEngine::GetAnimInfo(HCURSOR h)
{
    if (!m_pGetCursorFrameInfo)
        return {1, 0};
    DWORD rate = 0, count = 0;
    if (m_pGetCursorFrameInfo(h, 0, 0, &rate, &count))
    {
        if (count == 0)
            count = 1;
        int delay = (int)((rate * 1000) / 60);
        return {(int)count, delay < 10 ? 0 : delay};
    }
    return {1, 0};
}

CursorEngine::CursorEngine(SidebandServer &net)
    : m_net(net), m_token(0), m_hUser32(NULL), m_pGetCursorFrameInfo(NULL)
{
    Gdiplus::GdiplusStartupInput i;
    Gdiplus::GdiplusStartup(&m_token, &i, NULL);
    m_hUser32 = LoadLibraryA("user32.dll");
    if (m_hUser32)
        m_pGetCursorFrameInfo = (GETCURSORFRAMEINFO)GetProcAddress(m_hUser32, "GetCursorFrameInfo");
    mLastProcessTime = std::chrono::steady_clock::now();
    m_rawPixels.reserve(128 * 128);
    m_xorMask.reserve(128 * 128);
    m_pngBuffer.reserve(1024 * 50);
}

CursorEngine::~CursorEngine()
{
    FreeResources();
    if (m_hUser32)
        FreeLibrary(m_hUser32);
    Gdiplus::GdiplusShutdown(m_token);
}

void CursorEngine::ResetAfterDisplayChange()
{
    // 显示拓扑变化后，旧的 HMONITOR 句柄可能已失效，旧显示器的 DPI/档位缓存
    // 全部不可信。这里只作废缓存，真正的重新自适应由下一帧 CaptureAndSend 完成：
    // mLastMonitor=NULL 强制重查 DPI；mLastCursor=NULL 强制重发一帧当前光标。
    // 注意必须在捕获线程（WorkerLoop）调用。
    mLastCursor = NULL;
    mLastMonitor = NULL;
    m_isDpiChanging = false;
    mLastDpiCheckTime = std::chrono::steady_clock::time_point{};
    Logger::Get().Info("CursorEngine: 光标捕获状态已重置（新客户端/显示配置变化）");
}

void CursorEngine::CaptureAndSend()
{
    if (!m_net.HasClients())
    {
        mLastProcessTime = std::chrono::steady_clock::now();
        mLastCursor = NULL;
        return;
    }

    // 频率限制（30ms）
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastProcessTime).count() < 30)
        return;
    mLastProcessTime = now;

    CURSORINFO ci = {sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
        return;

    UINT currentDpi = GetCursorMonitorDPI();
    int expectedTierSize = GetExpectedSystemCursorSize(32, currentDpi);

    // 光标大小设置（注册表 CursorBaseSize）变化检测。
    // Windows 改指针大小滑块时是原位重建共享光标（IDC_ARROW 等）的位图内容：
    // HCURSOR 句柄值不变，仅靠下面的句柄比较永远检测不到 → 发出去的光标
    // 尺寸不跟随系统设置变化（问题 2 根因）。每 2 秒轮询一次注册表，
    // 变化时作废句柄缓存强制重捕，新尺寸的位图随下一帧发出。
    int targetSize = GetTargetSize();
    if (mLastTargetSize == -1)
        mLastTargetSize = targetSize;
    else if (targetSize != mLastTargetSize)
    {
        Logger::Get().Info("CursorEngine: 光标大小设置变更 ", mLastTargetSize, " -> ",
                           targetSize, "，强制重捕");
        mLastTargetSize = targetSize;
        mLastCursor = NULL;
    }

    if (mLastTierSize == -1)
        mLastTierSize = expectedTierSize;

    if (expectedTierSize != mLastTierSize)
    {
        if (!m_isDpiChanging)
        {
            m_isDpiChanging = true;
            m_dpiChangeStartTime = now;
        }
        else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_dpiChangeStartTime).count() > 500)
        {
            // 档位差异稳定超过 500ms：DPI 真的变了（典型场景就是切换显示器，
            // 两个屏幕缩放比例不同）。原版 windows-cursor-streamer 在这里
            // RestartApplication() 整进程重启来刷新资源；本进程是常驻服务，
            // 重启会断掉所有客户端连接，改为进程内软重置：接受新档位并作废
            // 旧光标缓存，下一帧按新档位正常捕获。
            // 不更新 mLastTierSize 的话这个分支每帧都 return，捕获会永久停止
            //（这正是"切换屏幕后捕获不到光标"的根因）。
            Logger::Get().Info("CursorEngine: 光标档位变更 ", mLastTierSize, " -> ",
                               expectedTierSize, "，重置捕获状态");
            mLastTierSize = expectedTierSize;
            m_isDpiChanging = false;
            mLastCursor = NULL;
        }
        return;
    }
    else
    {
        m_isDpiChanging = false;
    }

    if (ci.hCursor == mLastCursor)
        return;

    auto [frames, delay] = GetAnimInfo(ci.hCursor);

    ICONINFO ii = {0};
    if (!GetIconInfo(ci.hCursor, &ii))
        return;

    int orgW = 32, orgH = 32;
    BITMAP bmp;
    bool hasColor = false;

    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bmp), &bmp))
    {
        orgW = bmp.bmWidth;
        orgH = bmp.bmHeight;
        hasColor = true;
    }
    else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmp), &bmp))
    {
        orgW = bmp.bmWidth;
        orgH = bmp.bmHeight / 2;
    }

    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);

    int finalSizeW = orgW;
    int finalSizeH = orgH;

    // 安全检查：尺寸为 0 时 std::clamp(lo, 0, -1) 是未定义行为
    if (finalSizeW <= 0 || finalSizeH <= 0)
    {
        Logger::Get().Warning("CursorEngine: 异常光标尺寸 ", finalSizeW, "x", finalSizeH, "，跳过");
        return;
    }

    Logger::Get().Debug("[光标捕获] DPI:", currentDpi, "| 尺寸:", orgW, "x", orgH);

    int hotX = std::clamp(static_cast<int>(ii.xHotspot), 0, finalSizeW - 1);
    int hotY = std::clamp(static_cast<int>(ii.yHotspot), 0, finalSizeH - 1);

    int sheetW = finalSizeW;
    int sheetH = finalSizeH * frames;

    if (!RecreateResources(sheetW, sheetH))
        return;

    RECT allRc = {0, 0, sheetW, sheetH};

    // 黑底
    SelectObject(m_hMemDC, m_hBmpB);
    FillRect(m_hMemDC, &allRc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    for (int i = 0; i < frames; ++i)
        DrawIconEx(m_hMemDC, 0, i * finalSizeH, ci.hCursor, finalSizeW, finalSizeH, i, NULL, DI_NORMAL);

    // 白底
    SelectObject(m_hMemDC, m_hBmpW);
    FillRect(m_hMemDC, &allRc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    for (int i = 0; i < frames; ++i)
        DrawIconEx(m_hMemDC, 0, i * finalSizeH, ci.hCursor, finalSizeW, finalSizeH, i, NULL, DI_NORMAL);

    int totalPixels = sheetW * sheetH;
    m_rawPixels.resize(totalPixels);
    m_xorMask.resize(totalPixels);

    uint32_t *pOut = m_rawPixels.data();
    uint8_t *pMask = m_xorMask.data();
    const uint32_t *pB = (const uint32_t *)m_pBitsB;
    const uint32_t *pW = (const uint32_t *)m_pBitsW;

    // Pass 1: 颜色提取
    for (int i = 0; i < totalPixels; ++i)
    {
        uint32_t cB = pB[i];
        uint32_t cW = pW[i];
        uint8_t bb = (cB & 0xFF);
        uint8_t bg = ((cB >> 8) & 0xFF);
        uint8_t br = ((cB >> 16) & 0xFF);
        uint8_t wb = (cW & 0xFF);
        uint8_t wg = ((cW >> 8) & 0xFF);
        uint8_t wr = ((cW >> 16) & 0xFF);

        if (bg > 200 && wg < 50)
        {
            pOut[i] = 0xFFFFFFFF;
            pMask[i] = 1;
        }
        else
        {
            pMask[i] = 0;
            int dr = (int)wr - br;
            int dg = (int)wg - bg;
            int db = (int)wb - bb;
            if (dr < 0) dr = 0;
            if (dg < 0) dg = 0;
            if (db < 0) db = 0;
            int maxDiff = dr;
            if (dg > maxDiff) maxDiff = dg;
            if (db > maxDiff) maxDiff = db;
            uint8_t alpha = (uint8_t)(255 - maxDiff);
            if (alpha > 5)
                pOut[i] = (alpha << 24) | (br << 16) | (bg << 8) | bb;
            else
                pOut[i] = 0;
        }
    }

    // Pass 2: 智能描边
    int wMinus1 = sheetW - 1;
    int hMinus1 = sheetH - 1;
    for (int y = 0; y < sheetH; ++y)
    {
        uint32_t *rowOut = pOut + y * sheetW;
        uint8_t *rowMask = pMask + y * sheetW;
        uint8_t *rowMaskUp = (y > 0) ? (rowMask - sheetW) : NULL;
        uint8_t *rowMaskDown = (y < hMinus1) ? (rowMask + sheetW) : NULL;

        for (int x = 0; x < sheetW; ++x)
        {
            if (rowOut[x] != 0)
                continue;
            bool isBorder = false;
            if (x > 0 && rowMask[x - 1]) isBorder = true;
            else if (x < wMinus1 && rowMask[x + 1]) isBorder = true;
            else if (rowMaskUp && rowMaskUp[x]) isBorder = true;
            else if (rowMaskDown && rowMaskDown[x]) isBorder = true;
            if (isBorder)
                rowOut[x] = 0xFF000000;
        }
    }

    // CRC32
    size_t rawDataSize = m_rawPixels.size() * 4;
    uint32_t hash = CalculateCRC32(std::vector<uint8_t>(
        (uint8_t *)m_rawPixels.data(),
        (uint8_t *)m_rawPixels.data() + rawDataSize));

    // 缓存检查或 PNG 编码
    m_pngBuffer.clear();
    if (!m_net.GetCachedPng(hash, m_pngBuffer))
    {
        Gdiplus::Bitmap gdiBmp(sheetW, sheetH, PixelFormat32bppARGB);
        Gdiplus::BitmapData bd = {};
        Gdiplus::Rect r(0, 0, sheetW, sheetH);
        Gdiplus::Status st = gdiBmp.LockBits(&r, Gdiplus::ImageLockModeWrite,
                                              PixelFormat32bppARGB, &bd);
        if (st != Gdiplus::Ok || !bd.Scan0)
        {
            Logger::Get().Warning("CursorEngine: LockBits 失败 status=", (int)st);
            return;
        }
        memcpy(bd.Scan0, m_rawPixels.data(), rawDataSize);
        gdiBmp.UnlockBits(&bd);

        IStream *s = NULL;
        if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &s)) || !s)
        {
            Logger::Get().Warning("CursorEngine: 创建 PNG 流失败");
            return;
        }
        CLSID pngId = {};
        if (GetEncoderClsid(L"image/png", &pngId) < 0)
        {
            Logger::Get().Warning("CursorEngine: 找不到 PNG 编码器");
            s->Release();
            return;
        }
        st = gdiBmp.Save(s, &pngId, NULL);
        if (st != Gdiplus::Ok)
        {
            Logger::Get().Warning("CursorEngine: PNG 编码失败 status=", (int)st);
            s->Release();
            return;
        }

        STATSTG stg;
        s->Stat(&stg, STATFLAG_NONAME);
        std::vector<uint8_t> tempPng(stg.cbSize.LowPart);
        LARGE_INTEGER pos = {0};
        s->Seek(pos, STREAM_SEEK_SET, NULL);
        ULONG read;
        s->Read(tempPng.data(), (ULONG)tempPng.size(), &read);
        s->Release();

        m_net.CachePng(hash, tempPng);
        m_pngBuffer = std::move(tempPng);
    }

    if (!m_pngBuffer.empty())
    {
        mLastCursor = ci.hCursor;
        Logger::Get().Debug("CursorEngine: 发送光标 Hash=", hash);
        m_net.BroadcastCursor(hash, hotX, hotY, frames, delay, m_pngBuffer);
    }
}

// ============================================================
//                      CursorModule
// ============================================================

CursorModule::CursorModule(SidebandServer &server) : m_server(server)
{
    s_instance = this;
}

CursorModule::~CursorModule()
{
    Stop();
    if (s_instance == this)
        s_instance = nullptr;
}

bool CursorModule::Start()
{
    if (m_engine)
        return true; // 已启动

    m_engine = std::make_unique<CursorEngine>(m_server);
    m_exit = false;

    // 钩子消息循环线程（必须在有自己的消息循环的线程上安装 WH_MOUSE_LL/WH_KEYBOARD_LL/WinEventHook）
    m_hookThread = std::thread([this]()
                                { HookLoop(); });

    // 等待钩子线程安装完毕（最多 1 秒）
    for (int i = 0; i < 100 && m_hMouseHook == NULL && m_hKeyboardHook == NULL; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 工作线程
    m_workerThread = std::thread([this]()
                                 { WorkerLoop(); });

    // 文本光标监控线程
    m_textCursorThread = std::thread([this]()
                                     { TextCursorMonitorLoop(); });

    Logger::Get().Info("CursorModule: 已启动");
    return true;
}

void CursorModule::Stop()
{
    if (!m_engine)
        return;

    m_exit = true;
    m_cvCursorChanged.notify_all();
    m_cvTextCursor.notify_all();

    // 通知钩子线程退出消息循环
    if (m_hookThreadId != 0)
    {
        PostThreadMessage(m_hookThreadId, WM_QUIT, 0, 0);
    }

    if (m_hookThread.joinable())
        m_hookThread.join();

    if (m_workerThread.joinable())
        m_workerThread.join();
    if (m_textCursorThread.joinable())
        m_textCursorThread.join();

    m_engine.reset();
    Logger::Get().Info("CursorModule: 已停止");
}

void CursorModule::HookLoop()
{
    m_hookThreadId = GetCurrentThreadId();

    try
    {
    // 在此线程上安装钩子（要求消息循环）
    m_hWinEventHook = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL,
        CursorModule::WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    m_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, CursorModule::MouseProc, NULL, 0);
    if (!m_hMouseHook)
        Logger::Get().Error("CursorModule: 鼠标钩子安装失败");

    m_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, CursorModule::KeyboardProc, NULL, 0);
    if (!m_hKeyboardHook)
        Logger::Get().Error("CursorModule: 键盘钩子安装失败");
    else
        Logger::Get().Info("CursorModule: 文本光标追踪已启用 (鼠标+键盘+轮询)");

    // 消息循环（钩子回调在此线程派发）
    MSG msg;
    while (!m_exit && GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    }
    catch (const std::exception &e)
    {
        Logger::Get().Error("CursorModule: HookLoop 异常: ", e.what());
    }
    catch (...)
    {
        Logger::Get().Error("CursorModule: HookLoop 未知异常");
    }

    // 卸载钩子（必须在安装它的线程上）
    if (m_hWinEventHook)
    {
        UnhookWinEvent(m_hWinEventHook);
        m_hWinEventHook = NULL;
    }
    if (m_hMouseHook)
    {
        UnhookWindowsHookEx(m_hMouseHook);
        m_hMouseHook = NULL;
    }
    if (m_hKeyboardHook)
    {
        UnhookWindowsHookEx(m_hKeyboardHook);
        m_hKeyboardHook = NULL;
    }
    Logger::Get().Debug("CursorModule: 钩子线程退出");
}

void CursorModule::OnClientConnected(SidebandSession &)
{
    // 新客户端连入，强制刷新光标状态。
    // 只置标志：软重置统一由 WorkerLoop 在捕获线程执行，
    // 避免在网络线程直接调 m_engine->ResetAfterDisplayChange() 造成跨线程写。
    {
        std::lock_guard<std::mutex> l(m_mutexCursor);
        m_displayResetPending = true;
        m_cursorChanged = true;
    }
    m_cvCursorChanged.notify_one();
}

void CursorModule::OnClientDisconnected(SidebandSession &)
{
    // 单客户端断开不影响其他客户端；不需要特殊处理
}

void CursorModule::OnDisplayChanged()
{
    // WM_DISPLAYCHANGE（UI 线程）→ 只置标志唤醒 WorkerLoop，
    // 软重置在捕获线程执行（CursorEngine 的状态成员无同步保护）。
    {
        std::lock_guard<std::mutex> l(m_mutexCursor);
        m_displayResetPending = true;
        m_cursorChanged = true;
    }
    m_cvCursorChanged.notify_one();
}

void CursorModule::WorkerLoop()
{
    while (!m_exit)
    {
        try
        {
            {
                std::unique_lock<std::mutex> l(m_mutexCursor);
                m_cvCursorChanged.wait_for(l, std::chrono::milliseconds(33),
                                           [this] { return m_cursorChanged || m_exit; });
                if (m_exit)
                    break;
                // 显示拓扑变化 / 新客户端连入 → 先做软重置，再照常捕获
                bool reset = m_displayResetPending;
                m_displayResetPending = false;
                m_cursorChanged = false;
                l.unlock();
                if (reset && m_engine)
                {
                    m_engine->ResetAfterDisplayChange();
                }
            }
            if (m_engine)
                m_engine->CaptureAndSend();
        }
        catch (const std::exception &e)
        {
            Logger::Get().Error("CursorModule: WorkerLoop 异常: ", e.what());
        }
        catch (...)
        {
            Logger::Get().Error("CursorModule: WorkerLoop 未知异常");
        }
    }
}

void CursorModule::PokeTextCursor(bool force)
{
    // 由低级钩子回调调用 —— 只允许做常数时间的工作然后立刻返回
    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_textCursorPoke = true;
        if (force)
            m_textCursorPokeForce = true;
    }
    m_cvTextCursor.notify_one();
}

void CursorModule::TextCursorMonitorLoop()
{
    Logger::Get().Debug("CursorModule: 文本光标监控线程已启动");

    while (!m_exit)
    {
        try
        {
            bool force = false;
            {
                // 有输入事件就立刻醒，否则每 100ms 兜底轮询一次
                std::unique_lock<std::mutex> l(m_mutexTextCursor);
                m_cvTextCursor.wait_for(l, std::chrono::milliseconds(100),
                                        [this] { return m_textCursorPoke || m_exit; });
                if (m_exit)
                    break;
                force = m_textCursorPokeForce;
                m_textCursorPoke = false;
                m_textCursorPokeForce = false;
            }

            // UpdateTextCursorState 现在只在本线程调用，
            // 其内部的 static 缓存不再有跨线程竞争
            if (m_server.HasClients())
                UpdateTextCursorState(force);
        }
        catch (const std::exception &e)
        {
            Logger::Get().Error("CursorModule: TextCursorMonitorLoop 异常: ", e.what());
        }
        catch (...)
        {
            Logger::Get().Error("CursorModule: TextCursorMonitorLoop 未知异常");
        }
    }

    // UIA/COM 清理必须在本线程做（m_pUia 是跨进程 COM 接口指针，
    // 线程亲和；CoUninitialize 同理）。跑在循环之后的这里正是
    // m_textCursorThread 自己的退出路径。
    if (m_pUia)
    {
        m_pUia->Release();
        m_pUia = nullptr;
    }
    if (m_uiaComInited)
    {
        CoUninitialize();
        m_uiaComInited = false;
    }
}

// 判定插入符是否真的在闪。GUI_CARETBLINKING 未置位时 hwndCaret 可能
// 仍是一个残留的已注册插入符 —— 典型场景：应用失焦/失活后没调
// DestroyCaret，GetGUIThreadInfo 照样报出 rcCaret，但插入符实际不可见。
// 直接上报会持续报告一个并不存在的光标位置，所以这里过滤掉。
static bool IsCaretActuallyBlinking(const GUITHREADINFO &gti)
{
    return (gti.flags & GUI_CARETBLINKING) != 0;
}

// ============================================================
//   UI Automation 路径（阶段二：自绘 caret 应用兜底）
// ============================================================

// 确保 UIA 可用（惰性初始化，只在 m_textCursorThread 上调用）。
// 返回可用的 IUIAutomation*；不可用时返回 nullptr。
// 失败分两种：
//  - CoCreateInstance/Initialize 失败：置 m_uiaBroken，之后每 60s 才重试一次，
//    避免每次取词都在注定失败的 COM 初始化上空转。
//  - 只是本次调用失败：不动 m_uiaBroken，下一轮照常再试。
IUIAutomation *CursorModule::EnsureUia()
{
    if (m_pUia)
        return m_pUia;
    if (m_uiaBroken)
    {
        if (std::chrono::steady_clock::now() < m_uiaNextRetry)
            return nullptr;
        m_uiaBroken = false;  // 重试窗口到了，重新走完整初始化
    }

    // CoInitialize: UIA 内部自己持有 MTA，普通线程需要先初始化 COM。
    // STA 也可用（UIA 会内部转 MTA），MTA 减少一次隐式封送。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        { m_uiaBroken = true; m_uiaNextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(60); return nullptr; }
    // 只有本线程自己 init 成功（S_OK）才需要配对 CoUninitialize；
    // RPC_E_CHANGED_MODE 表示线程已有别的模式，释放责任不归我们。
    if (hr == S_OK)
        m_uiaComInited = true;

    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_pUia));
    if (FAILED(hr) || !m_pUia)
    {
        char hrBuf[32];
        std::snprintf(hrBuf, sizeof(hrBuf), "0x%08lX", (unsigned long)hr);
        Logger::Get().Warning("CursorModule: UIA 初始化失败 hr=", hrBuf);
        m_pUia = nullptr;
        m_uiaBroken = true;
        m_uiaNextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        return nullptr;
    }

    Logger::Get().Info("CursorModule: UIA 初始化成功（自绘插入符应用开始支持）");
    return m_pUia;
}

// 从 TextRange 的边界矩形数组里取"最后一行"的底边中点。
// 返回 false = 矩形数组为空/格式异常。
// GetBoundingRectangles 返回 VT_R8 SAFEARRAY，每矩形 4 个 double：
// [left, top, width, height]（注意是宽高不是 right/bottom）。
static bool RectsToCaretBottom(SAFEARRAY *psa, int &outX, int &outY, int &outHeight)
{
    if (!psa || SafeArrayGetDim(psa) != 1)  // GetDim 返回维度数（非 HRESULT）
        return false;

    LONG lb = 0, ub = 0;
    SafeArrayGetLBound(psa, 1, &lb);
    SafeArrayGetUBound(psa, 1, &ub);
    LONG count = ub - lb + 1;
    if (count < 4 || (count % 4) != 0)
        return false;
    LONG rects = count / 4;

    // 一次性锁定直取数据指针，避免逐元素 SafeArrayGetElement 的反复加锁。
    // 返回的指针指向 LBound 元素，元素连续存放，第 r 个矩形就是 data + r*4。
    double *data = nullptr;
    if (FAILED(SafeArrayAccessData(psa, (void HUGEP **)&data)) || !data)
        return false;

    double bestTop = 0.0, bestBottom = 0.0, bestLeft = 0.0, bestRight = 0.0;
    bool got = false;
    for (LONG r = 0; r < rects; r++)
    {
        const double *v = data + r * 4;
        double left = v[0], top = v[1], width = v[2], height = v[3];
        // 只要求行高非零：GetCaretRange 返回的是退化（空）选区，在不少实现
        //（含 Chromium）里就是零宽 + 行高的矩形，零宽是正常形态不能跳过。
        // 零宽时 left==right，中线自然退化为 left，正是插入点。
        if (height <= 0.0)
            continue;
        double bottom = top + height;
        // 取"最后一行"（caret 所在行）：行基线在下的就是当前输入行
        if (!got || bottom > bestBottom)
        {
            got = true;
            bestTop = top;
            bestBottom = bottom;
            bestLeft = left;
            bestRight = left + width;
        }
    }
    SafeArrayUnaccessData(psa);
    if (!got)
        return false;

    int x = (int)std::llround((bestLeft + bestRight) / 2.0);
    int y = (int)std::llround(bestBottom);
    int h = (int)std::llround(bestBottom - bestTop);
    if (h < 0)
        h = 0;
    outX = x;
    outY = y;
    outHeight = h;
    return true;
}

bool CursorModule::GetCaretViaUIA(int &outX, int &outY, int &outHeight)
{
    outX = outY = outHeight = 0;

    IUIAutomation *uia = EnsureUia();
    if (!uia)
        return false;

    // 1. 聚焦元素。FocusChanged 可能后到，捕获点位置为准 —— GetFocusedElement
    //    直查实时状态，比事件缓存可靠。
    IUIAutomationElement *pFocus = nullptr;
    if (FAILED(uia->GetFocusedElement(&pFocus)) || !pFocus)
        return false;

    bool ok = false;
    int x = 0, y = 0, h = 0;

    // 2. TextPattern2::GetCaretRange —— Chromium、Firefox、WinUI、记事本
    //    等现代文本栈都实现。取不到再退化到 selection（第 3 步）。
    //    GetPattern 对不支持的 pattern 可能返回 S_OK 但指针为空，两种都要防。
    ITextPattern2 *pPattern2 = nullptr;
    if (SUCCEEDED(pFocus->GetPattern(UIA_TextPattern2, &pPattern2)) && pPattern2)
    {
        // 场景：SelectAll 后 caret 在文末 —— 确保取的是 caret 而非 selection 头。
        BOOL isActive = FALSE;
        IUIAutomationTextRange *pRange = nullptr;
        if (SUCCEEDED(pPattern2->GetCaretRange(&isActive, &pRange)) && pRange)
        {
            SAFEARRAY *psa = nullptr;
            if (SUCCEEDED(pRange->GetBoundingRectangles(&psa)))
            {
                if (RectsToCaretBottom(psa, x, y, h))
                    ok = true;
                if (psa)
                    SafeArrayDestroy(psa);
            }
            pRange->Release();
        }
        pPattern2->Release();
    }

    // 3. 退化路径：无 TextPattern2 时取选区矩形。GetSelection 返回的是
    //    IUIAutomationTextRangeArray 的 SAFEARRAY（不是单个 range）。
    //    【闸门】只接受控件类型为 Edit 的焦点元素：浏览器里点击页面文本
    //    会留下 DOM 选区，Document 控件的 TextPattern 会把它报成 selection
    //    —— 那不是 caret，照单全收就等于从 UIA 后门放回了假数据
    //    （阶段一刚清理掉的那种）。退化场景（无 TextPattern2 的文本栈，
    //    如部分 Qt 版本）真正的输入框控件类型就是 Edit。
    if (!ok)
    {
        ITextPattern *pPattern = nullptr;
        if (SUCCEEDED(pFocus->GetPattern(UIA_TextPattern, &pPattern)) && pPattern)
        {
            VARIANT vtType;
            VariantInit(&vtType);
            bool isEdit = false;
            if (SUCCEEDED(pFocus->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &vtType)) &&
                vtType.vt == VT_I4 && vtType.lVal == UIA_EditControlTypeId)
                isEdit = true;
            VariantClear(&vtType);

            if (isEdit)
            {
                SAFEARRAY *psaRanges = nullptr;
                if (SUCCEEDED(pPattern->GetSelection(&psaRanges)) && psaRanges)
                {
                    LONG lb = 0, ub = 0;
                    SafeArrayGetLBound(psaRanges, 1, &lb);
                    SafeArrayGetUBound(psaRanges, 1, &ub);
                    for (LONG i = lb; i <= ub && !ok; i++)
                    {
                        IUIAutomationTextRange *pRange = nullptr;
                        if (FAILED(SafeArrayGetElement(psaRanges, &i, &pRange)) || !pRange)
                            continue;
                        SAFEARRAY *psa = nullptr;
                        if (SUCCEEDED(pRange->GetBoundingRectangles(&psa)))
                        {
                            if (RectsToCaretBottom(psa, x, y, h))
                                ok = true;
                            if (psa)
                                SafeArrayDestroy(psa);
                        }
                        pRange->Release();
                    }
                    SafeArrayDestroy(psaRanges);
                }
            }
            pPattern->Release();
        }
    }

    pFocus->Release();

    if (!ok)
        return false;

    outX = x;
    outY = y;
    outHeight = h;
    return true;
}

bool CursorModule::GetCaretViaWin32(int &outX, int &outY, int &outHeight)
{
    // 必须取前台窗口所属线程：GetGUIThreadInfo(0) 拿的是"调用线程"的 GUI 状态，
    // 而本函数跑在 m_textCursorThread 工作线程上，没有 GUI 消息队列，
    // 永远拿不到 caret/focus。前台窗口的线程才有这些信息。
    HWND hwndForeground = GetForegroundWindow();
    if (!hwndForeground)
        return false;
    DWORD foregroundThreadId = GetWindowThreadProcessId(hwndForeground, nullptr);
    if (foregroundThreadId == 0)
        return false;

    GUITHREADINFO gti = {sizeof(GUITHREADINFO)};
    if (!GetGUIThreadInfo(foregroundThreadId, &gti))
        return false;

    // 只认真正的 Win32 系统插入符。
    //
    // 历史教训：旧版这里还有一条"焦点窗口内有鼠标 → 返回鼠标位置"的兜底，
    // 注释写着"适用于自绘光标的应用，如 Chrome/Electron"。但它与输入框毫无
    // 关系：打字时鼠标通常停在别处，发出去的 Y 是错的；没在输入时（鼠标
    // 恰好停在焦点窗口内）还会源源不断地伪造"文本光标"状态，并经由
    // m_textCursorActive 让键盘钩子持续自我续命。自绘 caret 的应用
    //（Chromium/Electron、UWP/WinUI、Qt、Flutter、Java）hwndCaret 为
    // NULL —— 正确行为是落到 UIA 路径（GetCaretViaUIA）。
    if (!gti.hwndCaret)
        return false;

    // 插入符必须在闪（见 IsCaretActuallyBlinking 的注释）。
    if (!IsCaretActuallyBlinking(gti))
        return false;

    // 高度优先取 bottom - top；注册了零高/畸形矩形的个别应用给 0（未知）。
    // 注意不做 "left >= 0" 检查：插入符横向滚出客户区（left 为负）时
    // Y 仍然是有效的输入行位置，按"无插入符"处理反而丢掉了真实状态。
    int height = gti.rcCaret.bottom - gti.rcCaret.top;
    if (height < 0)
        height = 0;

    // 基准取底边（bottom）：下游"别让软键盘挡住当前输入行"要避开的是
    // 行底而非行顶。X 取插入符中线，仅供需要水平位置的调用方使用。
    POINT caretPos = {gti.rcCaret.left + (gti.rcCaret.right - gti.rcCaret.left) / 2,
                      gti.rcCaret.bottom};
    if (!ClientToScreen(gti.hwndCaret, &caretPos))
        return false;

    outX = caretPos.x;
    outY = caretPos.y;
    outHeight = height;
    return true;
}

void CursorModule::UpdateTextCursorState(bool forceUpdate)
{
    // 本函数只跑在 m_textCursorThread（见 TextCursorMonitorLoop），
    // static 缓存无跨线程竞争。
    static int lastCaretX = -1;
    static int lastCaretY = -1;

    int currentState = -1;
    int caretX = 0, caretY = 0;
    int caretHeight = 0;
    int caretSource = SidebandProtocol::CARET_SOURCE_NONE;

    // 两级取词：Win32 系统 caret 优先（零成本、零风险），失败再走 UIA
    //（跨进程 COM，可能阻塞，但能覆盖自绘 caret 的现代应用栈）。
    if (GetCaretViaWin32(caretX, caretY, caretHeight))
    {
        caretSource = SidebandProtocol::CARET_SOURCE_WIN32;
    }
    else if (GetCaretViaUIA(caretX, caretY, caretHeight))
    {
        caretSource = SidebandProtocol::CARET_SOURCE_UIA;
    }

    bool haveCaret = (caretSource != SidebandProtocol::CARET_SOURCE_NONE);
    if (haveCaret)
    {
        if (!forceUpdate && caretX == lastCaretX && caretY == lastCaretY)
            return;
        lastCaretX = caretX;
        lastCaretY = caretY;

        POINT pt = {caretX, caretY};
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(MONITORINFO)};
        if (GetMonitorInfo(hMon, &mi))
        {
            int monitorHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
            int relativeY = caretY - mi.rcMonitor.top;
            currentState = (int)((relativeY * 10000.0f) / monitorHeight);
            currentState = std::clamp(currentState, 0, 10000);
        }
        else
        {
            // 拿不到显示器信息时本包只能按"无插入符"发送，
            // 不允许出现"-1 却带着高度/来源"的自相矛盾包。
            caretHeight = 0;
            caretSource = SidebandProtocol::CARET_SOURCE_NONE;
            currentState = -1;
        }
        m_textCursorActive = true;
    }
    else
    {
        lastCaretX = -1;
        lastCaretY = -1;
        m_textCursorActive = false;
    }

    int lastState = m_lastSentState.load();
    bool stateChanged = forceUpdate;
    if (!stateChanged)
    {
        if (currentState == -1 && lastState != -1)
            stateChanged = true;
        else if (currentState != -1 && std::abs(currentState - lastState) > 50)
            stateChanged = true;
    }

    if (stateChanged)
    {
        if (currentState == -1)
            Logger::Get().Debug("[文本光标] 退出输入状态");
        else
            Logger::Get().Debug("[文本光标] Y 轴位置:", currentState / 100.0f, "%");

        m_server.BroadcastTextCursorState(currentState, caretHeight, caretSource);
        m_lastSentState.store(currentState);
    }
}

// ============================================================
//                      静态钩子回调
// ============================================================

void CALLBACK CursorModule::WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id, LONG, DWORD, DWORD)
{
    if (id == OBJID_CURSOR && s_instance)
    {
        std::lock_guard<std::mutex> l(s_instance->m_mutexCursor);
        s_instance->m_cursorChanged = true;
        s_instance->m_cvCursorChanged.notify_one();
    }
}

// 低级钩子回调铁律：常数时间内返回。
// 它跑在全系统每一次鼠标事件上，一旦单次超过 LowLevelHooksTimeout
//（HKCU\Control Panel\Desktop\LowLevelHooksTimeout，默认 300ms），
// Windows 会不声不响地把这个钩子摘掉，而且期间整个系统的输入都会卡顿。
// 原实现在这里同步调用 UpdateTextCursorState()，里面有 GetGUIThreadInfo /
// GetWindowRect 等可能阻塞在别的进程上的调用 —— 现在只置标志并唤醒工作线程。
LRESULT CALLBACK CursorModule::MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && wParam == WM_LBUTTONUP)
    {
        s_instance->PokeTextCursor(true);
    }
    return CallNextHookEx(s_instance ? s_instance->m_hMouseHook : NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK CursorModule::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_textCursorActive)
    {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            s_instance->PokeTextCursor(false);
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_hKeyboardHook : NULL, nCode, wParam, lParam);
}
