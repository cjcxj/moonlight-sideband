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

#include <algorithm>
#include <cstring>
#include <cmath>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

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
    static auto s_lastCheck = std::chrono::steady_clock::now();
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

void CursorEngine::ResetState()
{
    mLastCursor = NULL;
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

    if (mLastTierSize == -1)
        mLastTierSize = expectedTierSize;

    if (expectedTierSize != mLastTierSize)
    {
        if (!m_isDpiChanging)
        {
            m_isDpiChanging = true;
            m_dpiChangeStartTime = now;
        }
        else
        {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_dpiChangeStartTime).count() > 500)
            {
                // DPI 变化重启逻辑交给上层处理；这里仅跳过本帧
                Logger::Get().Info("CursorEngine: 检测到 DPI 变化，跳过本帧");
            }
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
            Logger::Get().Error("CursorEngine: LockBits 失败 status=", (int)st);
            return;
        }
        memcpy(bd.Scan0, m_rawPixels.data(), rawDataSize);
        gdiBmp.UnlockBits(&bd);

        IStream *s = NULL;
        if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &s)) || !s)
        {
            Logger::Get().Error("CursorEngine: 创建 PNG 流失败");
            return;
        }
        CLSID pngId = {};
        if (GetEncoderClsid(L"image/png", &pngId) < 0)
        {
            Logger::Get().Error("CursorEngine: 找不到 PNG 编码器");
            s->Release();
            return;
        }
        st = gdiBmp.Save(s, &pngId, NULL);
        if (st != Gdiplus::Ok)
        {
            Logger::Get().Error("CursorEngine: PNG 编码失败 status=", (int)st);
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
    Logger::Get().Info("CursorModule: 钩子线程退出");
}

void CursorModule::OnClientConnected(SidebandSession &session)
{
    // 新客户端连入，强制刷新光标状态
    if (m_engine)
    {
        m_engine->ResetState();
        Logger::Get().Info("CursorModule: 新客户端连接，强制刷新光标状态");
    }
    {
        std::lock_guard<std::mutex> l(m_mutexCursor);
        m_cursorChanged = true;
    }
    m_cvCursorChanged.notify_one();
}

void CursorModule::OnClientDisconnected(SidebandSession &session)
{
    // 单客户端断开不影响其他客户端；不需要特殊处理
}

void CursorModule::WorkerLoop()
{
    while (!m_exit)
    {
        {
            std::unique_lock<std::mutex> l(m_mutexCursor);
            m_cvCursorChanged.wait_for(l, std::chrono::milliseconds(33),
                                       [this] { return m_cursorChanged || m_exit; });
            if (m_exit)
                break;
            m_cursorChanged = false;
        }
        if (m_engine)
            m_engine->CaptureAndSend();
    }
}

void CursorModule::TextCursorMonitorLoop()
{
    Logger::Get().Info("CursorModule: 文本光标监控线程已启动");
    while (!m_exit)
    {
        if (m_server.HasClients())
        {
            UpdateTextCursorState();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool CursorModule::GetCaretScreenPosition(int &outX, int &outY)
{
    GUITHREADINFO gti = {sizeof(GUITHREADINFO)};
    if (GetGUIThreadInfo(0, &gti))
    {
        if (gti.hwndCaret && gti.rcCaret.left >= 0)
        {
            POINT caretPos = {gti.rcCaret.left, gti.rcCaret.top};
            if (ClientToScreen(gti.hwndCaret, &caretPos))
            {
                outX = caretPos.x;
                outY = caretPos.y;
                return true;
            }
        }
        if (gti.hwndFocus)
        {
            RECT focusRect;
            if (GetWindowRect(gti.hwndFocus, &focusRect))
            {
                POINT mousePos;
                if (GetCursorPos(&mousePos))
                {
                    if (PtInRect(&focusRect, mousePos))
                    {
                        outX = mousePos.x;
                        outY = mousePos.y;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void CursorModule::UpdateTextCursorState(bool forceUpdate)
{
    static int lastCaretX = -1;
    static int lastCaretY = -1;

    int currentState = -1;
    int caretX = 0, caretY = 0;

    if (GetCaretScreenPosition(caretX, caretY))
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

        m_server.BroadcastTextCursorState(currentState);
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

LRESULT CALLBACK CursorModule::MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && wParam == WM_LBUTTONUP)
    {
        s_instance->UpdateTextCursorState(true);
    }
    return CallNextHookEx(s_instance ? s_instance->m_hMouseHook : NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK CursorModule::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_textCursorActive)
    {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            s_instance->UpdateTextCursorState();
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_hKeyboardHook : NULL, nCode, wParam, lParam);
}
