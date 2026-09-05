/**
 * caret_probe - 文本光标（插入符）上报测试客户端
 *
 * 用途：验证 moonlight-sideband 的文本插入符追踪（CmdID=2 老格式包）。
 * 连上服务端后持续解析并打印每个文本光标包的：
 *   YPercent（插入符底边在显示器内的纵向百分比，-1=无插入符）
 *   Height  （插入符高度像素，包尾保留字段 1）
 *   Source （来源标记：0=无 1=Win32 2=MSAA 3=UIA，包尾保留字段 2）
 *   Δ      （距上一个文本光标包的毫秒数 —— 观察事件驱动 vs 轮询节奏）
 *
 * 普通光标包（PNG 帧）静默计数，不刷屏。
 *
 * 构建（CMake，推荐）：
 *   cmake -S . -B build -DSIDEBAND_BUILD_CARET_PROBE=ON
 *   cmake --build build --config Release
 *   build\Release\caret_probe.exe [host] [port]     （默认 127.0.0.1 5005）
 *
 * 构建（VS 开发命令提示符单命令）：
 *   cl /utf-8 /EHsc tools\caret_probe.cpp ws2_32.lib
 *
 * 用法：跑起来后照 README/提交说明里的场景清单操作前台应用即可。
 * 退出：q + 回车。
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

static const char *SourceName(int32_t s)
{
    switch (s)
    {
    case 0: return "无";
    case 1: return "Win32";
    case 2: return "MSAA(预留)";
    case 3: return "UIA";
    default: return "?";
    }
}

static void PrintNow()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::printf("[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? std::atoi(argv[2]) : 5005;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup 失败\n");
        return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        std::printf("socket 创建失败\n");
        return 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        std::printf("地址非法: %s\n", host);
        return 1;
    }
    if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        std::printf("连接 %s:%d 失败 (WSA=%d)——服务端起了吗？端口对吗？\n",
                    host, port, WSAGetLastError());
        return 1;
    }
    // 连上后转非阻塞，用 select 轮询，配合 _kbhit 退出
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    std::printf("已连接 %s:%d，监听文本光标包（普通光标帧静默计数）...\n", host, port);
    std::printf("退出: q + 回车\n\n");

    std::vector<uint8_t> buf;   // 接收缓冲（跨 recv 的半帧拼接）
    uint8_t chunk[65536];
    long cursorPktCount = 0;
    uint64_t lastTextPkt = 0;
    int32_t lastY = -999;      // -999 = 尚未见过任何包
    bool inTyping = false;     // 状态展示（进入/退出输入状态）

    while (true)
    {
        while (_kbhit())
        {
            if (std::getchar() == 'q')
                goto done;
        }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv = {0, 200000};  // 200ms
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (r > 0)
        {
            int n = recv(s, (char *)chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                std::printf("\n连接断开（服务端退出？）\n");
                break;
            }
            buf.insert(buf.end(), chunk, chunk + n);

            // 解析所有完整帧：[BodyLen(4)] Body
            while (buf.size() >= 4)
            {
                uint32_t bodyLen;
                std::memcpy(&bodyLen, buf.data(), 4);
                if (bodyLen == 0 || bodyLen > 16u * 1024 * 1024)
                {
                    std::printf("非法 bodyLen=%u，字节流错位，退出\n", bodyLen);
                    goto done;
                }
                if (buf.size() < 4 + (size_t)bodyLen)
                    break;  // 半包，等下一轮 recv

                const uint8_t *body = buf.data() + 4;
                uint32_t magic;
                std::memcpy(&magic, body, 4);

                if (magic == 0xFFFFFFFF)
                {
                    // 控制指令/文本光标（老格式 CmdID=2：bodyLen==20）
                    if (bodyLen == 20)
                    {
                        uint32_t cmdId;
                        std::memcpy(&cmdId, body + 4, 4);
                        if (cmdId == 2)
                        {
                            int32_t y, h, src;
                            std::memcpy(&y, body + 8, 4);
                            std::memcpy(&h, body + 12, 4);
                            std::memcpy(&src, body + 16, 4);

                            uint64_t now = GetTickCount64();
                            uint64_t delta = lastTextPkt ? (now - lastTextPkt) : 0;
                            lastTextPkt = now;

                            PrintNow();
                            if (y < 0)
                            {
                                std::printf("退出输入状态（无插入符）");
                                inTyping = false;
                            }
                            else
                            {
                                std::printf("%s Y=%d.%02d%% 高度=%dpx 来源=%s",
                                            inTyping ? "更新" : "进入输入状态",
                                            y / 100, y % 100, h, SourceName(src));
                                // 下游避让参考：软键盘上沿应位于
                                // (y% - 高度对应百分比) 之上，即别盖住整行
                                inTyping = true;
                            }
                            if (delta)
                                std::printf(" | 距上包 %llums", (unsigned long long)delta);
                            std::printf("\n");
                        }
                    }
                    // 其他控制指令（Heartbeat/Hello 等）忽略
                }
                else
                {
                    cursorPktCount++;  // 普通光标包，静默计数
                }

                buf.erase(buf.begin(), buf.begin() + 4 + (int)bodyLen);
            }
        }
        else if (r == SOCKET_ERROR)
        {
            std::printf("select 错误 %d\n", WSAGetLastError());
            break;
        }
    }

done:
    closesocket(s);
    WSACleanup();
    std::printf("\n共收到普通光标帧 %ld 个（静默计数）\n", cursorPktCount);
    return 0;
}
