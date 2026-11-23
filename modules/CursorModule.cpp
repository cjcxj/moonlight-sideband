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
#include "cursor/CaretEngine.hpp"
#include "Logger.hpp"
#include "SidebandProtocol.hpp"
#include "Win32Util.hpp"

#include <windows.h>
#include <shellscalingapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef EVENT_OBJECT_TEXTSELECTIONCHANGED
#define EVENT_OBJECT_TEXTSELECTIONCHANGED 0x8014
#endif
#ifndef EVENT_OBJECT_IME_SHOW
#define EVENT_OBJECT_IME_SHOW 0x8027
#endif
#ifndef EVENT_OBJECT_IME_HIDE
#define EVENT_OBJECT_IME_HIDE 0x8028
#endif
#ifndef EVENT_OBJECT_IME_CHANGE
#define EVENT_OBJECT_IME_CHANGE 0x8029
#endif

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

CursorModule::CursorModule(SidebandServer &server,
                           bool allowImeFallback,
                           bool allowControlBoxForLegacy)
    : m_server(server),
      m_allowImeFallback(allowImeFallback),
      m_allowControlBoxForLegacy(allowControlBoxForLegacy)
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
    Caret::CaretOptions opts;
    opts.allowImeFallback = m_allowImeFallback;
    opts.allowControlBoxForLegacy = m_allowControlBoxForLegacy;
    m_caretEngine = std::make_unique<Caret::CaretEngine>(opts);
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

    m_caretEngine.reset();
    m_engine.reset();
    Logger::Get().Info("CursorModule: 已停止");
}

void CursorModule::HookLoop()
{
    m_hookThreadId = GetCurrentThreadId();

    try
    {
        // 1. 鼠标光标形状变化钩子
        m_hWinEventHook = SetWinEventHook(
            EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL,
            CursorModule::WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        // 2. 文本插入符事件钩子（窄区间 + 回调首行硬过滤）
        m_hCaretFgHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
            CursorModule::CaretWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        m_hCaretObjHook = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, NULL,
            CursorModule::CaretWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        m_hCaretTextHook = SetWinEventHook(
            EVENT_OBJECT_TEXTSELECTIONCHANGED, EVENT_OBJECT_IME_CHANGE, NULL,
            CursorModule::CaretWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        m_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, CursorModule::MouseProc, NULL, 0);
        if (!m_hMouseHook)
            Logger::Get().Error("CursorModule: 鼠标钩子安装失败");

        m_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, CursorModule::KeyboardProc, NULL, 0);
        if (!m_hKeyboardHook)
            Logger::Get().Error("CursorModule: 键盘钩子安装失败");
        else
            Logger::Get().Info("CursorModule: 文本光标追踪已启用 (WinEvent + 鼠标/键盘 + 兜底)");

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
    if (m_hCaretTextHook)
    {
        UnhookWinEvent(m_hCaretTextHook);
        m_hCaretTextHook = NULL;
    }
    if (m_hCaretObjHook)
    {
        UnhookWinEvent(m_hCaretObjHook);
        m_hCaretObjHook = NULL;
    }
    if (m_hCaretFgHook)
    {
        UnhookWinEvent(m_hCaretFgHook);
        m_hCaretFgHook = NULL;
    }
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
    {
        std::lock_guard<std::mutex> l(m_mutexCursor);
        m_displayResetPending = true;
        m_cursorChanged = true;
    }
    m_cvCursorChanged.notify_one();

    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_textCursorResetPending = true;
        m_textCursorPoke = true;
        m_textCursorPokeForce = true;
    }
    m_cvTextCursor.notify_one();
}

void CursorModule::OnClientDisconnected(SidebandSession &)
{
}

void CursorModule::OnDisplayChanged()
{
    {
        std::lock_guard<std::mutex> l(m_mutexCursor);
        m_displayResetPending = true;
        m_cursorChanged = true;
    }
    m_cvCursorChanged.notify_one();

    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_textCursorResetPending = true;
        m_textCursorPoke = true;
        m_textCursorPokeForce = true;
    }
    m_cvTextCursor.notify_one();
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

void CursorModule::PokeTextCursor(bool force, bool fromKeyboard)
{
    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_textCursorPoke = true;
        if (force)
            m_textCursorPokeForce = true;
        if (fromKeyboard)
            m_lastKeyboardInputMs = GetTickCount64();
    }
    m_cvTextCursor.notify_one();
}

void CursorModule::OnCaretWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild)
{
    // 首行硬过滤：绝不允许桌面窗口位置抖动把工作线程打醒
    if (event == EVENT_SYSTEM_FOREGROUND)
    {
        m_trackedFgHwnd.store(hwnd, std::memory_order_relaxed);
        PokeTextCursor(true);
        return;
    }

    if (event == EVENT_OBJECT_FOCUS)
    {
        if (idObject != OBJID_CLIENT && idObject != CHILDID_SELF)
            return;
        m_trackedFocusHwnd.store(hwnd, std::memory_order_relaxed);
        PokeTextCursor(true);
        return;
    }

    if (event == EVENT_OBJECT_TEXTSELECTIONCHANGED)
    {
        PokeTextCursor(false);
        return;
    }

    if (event == EVENT_OBJECT_IME_SHOW || event == EVENT_OBJECT_IME_CHANGE)
    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_imeVisible = true;
        if (hwnd)
            m_imeHwnd = hwnd;
        m_lastImeEventMs = GetTickCount64();
        m_pendingImeEvent = true;
        m_textCursorPoke = true;
        m_cvTextCursor.notify_one();
        return;
    }

    if (event == EVENT_OBJECT_IME_HIDE)
    {
        std::lock_guard<std::mutex> l(m_mutexTextCursor);
        m_imeVisible = false;
        m_imeHwnd = nullptr;
        m_textCursorPoke = true;
        m_cvTextCursor.notify_one();
        return;
    }

    if (event == EVENT_OBJECT_SHOW ||
        event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        if (idObject == OBJID_CARET)
        {
            std::lock_guard<std::mutex> l(m_mutexTextCursor);
            m_pendingCaretEvent = true;
            m_lastCaretEventHwnd = hwnd;
            m_lastCaretEventObj = idObject;
            m_lastCaretEventChild = idChild;
            m_textCursorPoke = true;
            m_cvTextCursor.notify_one();
            return;
        }

        if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW && hwnd)
        {
            HWND fg = m_trackedFgHwnd.load(std::memory_order_relaxed);
            HWND foc = m_trackedFocusHwnd.load(std::memory_order_relaxed);
            if (hwnd == fg || hwnd == foc)
            {
                PokeTextCursor(false);
            }
        }
    }
}

// 覆盖率排查用：把「哪个程序、哪个窗口类」直接写进文本光标日志，
// 否则一轮应用扫下来只能靠时间戳猜是谁。
// 只读进程名与类名，不碰任何文本内容；级别没开到 DEBUG 时一笔系统调用都不做。
// hwnd 传的是插入符归属窗口（IME 锚点路径填的也是被输入程序的窗口，不是候选框自己），
// 所以 src=5 这行报出来的仍是「你正在敲字的那个应用」。
static std::string CaretOwnerTag(HWND hwnd)
{
    if (Logger::Get().GetLogLevel() > LogLevel::DEBUG)
        return std::string();
    if (!hwnd || !IsWindow(hwnd))
        return "<->";

    wchar_t cls[96] = L"";
    GetClassNameW(hwnd, cls, 96);

    std::string exe = "?";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid)
    {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess)
        {
            wchar_t path[512] = L"";
            DWORD len = 512;
            if (QueryFullProcessImageNameW(hProcess, 0, path, &len))
            {
                const std::wstring full(path, len);
                const size_t slash = full.find_last_of(L"\\/");
                exe = Win32Util::WideToUtf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
            }
            CloseHandle(hProcess);
        }
    }
    return exe + "|" + Win32Util::WideToUtf8(cls);
}

void CursorModule::TextCursorMonitorLoop()
{
    Logger::Get().Debug("CursorModule: 文本光标监控线程已启动");

    while (!m_exit)
    {
        try
        {
            bool force = false;
            bool reset = false;
            bool caretEvt = false;
            bool imeEvt = false;
            bool imeVisible = false;
            HWND caretHwnd = nullptr;
            LONG caretObj = 0;
            LONG caretChild = 0;
            HWND imeHwnd = nullptr;
            uint64_t lastImeMs = 0;
            uint64_t lastKbMs = 0;

            {
                // 活跃输入态 250ms 校对滚动/状态；无文本焦点时 1000ms 低频休眠（事件随时毫秒级唤醒）
                const auto waitDur = m_textCursorActive.load(std::memory_order_relaxed)
                                         ? std::chrono::milliseconds(250)
                                         : std::chrono::milliseconds(1000);
                std::unique_lock<std::mutex> l(m_mutexTextCursor);
                m_cvTextCursor.wait_for(l, waitDur,
                                        [this] { return m_textCursorPoke || m_exit; });
                if (m_exit)
                    break;

                // 短暂事件合并去抖（12ms），防止连续高频 LOCATIONCHANGE/打字脉冲重复触发跨进程 UIA
                if (m_textCursorPoke && !m_textCursorPokeForce && !m_textCursorResetPending)
                {
                    l.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(12));
                    l.lock();
                    if (m_exit)
                        break;
                }

                force = m_textCursorPokeForce;
                reset = m_textCursorResetPending;
                caretEvt = m_pendingCaretEvent;
                imeEvt = m_pendingImeEvent;
                imeVisible = m_imeVisible;
                caretHwnd = m_lastCaretEventHwnd;
                caretObj = m_lastCaretEventObj;
                caretChild = m_lastCaretEventChild;
                imeHwnd = m_imeHwnd;
                lastImeMs = m_lastImeEventMs;
                lastKbMs = m_lastKeyboardInputMs;

                m_textCursorPoke = false;
                m_textCursorPokeForce = false;
                m_textCursorResetPending = false;
                m_pendingCaretEvent = false;
                m_pendingImeEvent = false;
            }

            if (reset && m_caretEngine)
                m_caretEngine->ResetState();

            if (!m_server.HasClients() || !m_caretEngine)
                continue;

            Caret::CaretContextSnapshot hint;
            hint.triggeredByCaretEvent = caretEvt;
            hint.triggeredByImeEvent = imeEvt;
            hint.lastImeEventMs = lastImeMs;
            hint.lastKeyboardInputMs = lastKbMs;
            if (imeVisible && imeHwnd && IsWindow(imeHwnd) && IsWindowVisible(imeHwnd))
            {
                RECT rc{};
                if (GetWindowRect(imeHwnd, &rc))
                {
                    hint.imeVisible = true;
                    hint.imeWindowRect = Caret::PixelRect{rc.left, rc.top, rc.right, rc.bottom};
                }
            }

            auto out = m_caretEngine->DetectAndEvaluate(&hint, caretHwnd, caretObj, caretChild, force);
            m_textCursorActive.store(out.result.valid, std::memory_order_relaxed);

            if (out.result.valid)
            {
                m_trackedFgHwnd.store(GetForegroundWindow(), std::memory_order_relaxed);
                m_trackedFocusHwnd.store(reinterpret_cast<HWND>(out.result.hwnd), std::memory_order_relaxed);
            }

            if (out.publish.sendLegacy)
            {
                const HWND ownerHwnd = out.result.valid
                                           ? reinterpret_cast<HWND>(out.result.hwnd)
                                           : GetForegroundWindow();
                const std::string owner = CaretOwnerTag(ownerHwnd);

                if (out.publish.legacy.yPercent == -1)
                    Logger::Get().Debug("[文本光标] 退出输入状态 ", owner);
                else
                    Logger::Get().Debug("[文本光标] Y=", out.publish.legacy.yPercent / 100.0f,
                                        "% src=", out.publish.legacy.source,
                                        " h=", out.publish.legacy.height,
                                        " @", owner);

                m_server.BroadcastTextCursorState(out.publish.legacy.yPercent,
                                                  out.publish.legacy.height,
                                                  out.publish.legacy.source);
            }

            if (out.publish.sendExt && !out.extJson.empty())
            {
                m_server.BroadcastTextCursorExt(out.extJson);
            }
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

    if (m_caretEngine)
        m_caretEngine->ShutdownProviders();
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

void CALLBACK CursorModule::CaretWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                              LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (s_instance)
        s_instance->OnCaretWinEvent(event, hwnd, idObject, idChild);
}

LRESULT CALLBACK CursorModule::MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && wParam == WM_LBUTTONUP)
    {
        s_instance->PokeTextCursor(true, false);
    }
    return CallNextHookEx(s_instance ? s_instance->m_hMouseHook : NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK CursorModule::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance)
    {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            // 若当前在文本态直接唤醒；非文本态时仅记录按键时间（由焦点/IME WinEvent 主动唤醒，避免游戏按键空转）
            if (s_instance->m_textCursorActive.load(std::memory_order_relaxed))
            {
                s_instance->PokeTextCursor(false, true);
            }
            else if (auto *kb = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam))
            {
                // Tab / Enter / F6 / L 等常见切焦点键也允许触发一次低成本检查
                if (kb->vkCode == VK_TAB || kb->vkCode == VK_RETURN || kb->vkCode == VK_F6)
                    s_instance->PokeTextCursor(false, true);
            }
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_hKeyboardHook : NULL, nCode, wParam, lParam);
}
