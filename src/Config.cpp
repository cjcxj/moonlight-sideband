#include "Config.hpp"
#include "Win32Util.hpp"
#include "Logger.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>   // atoi / _strtoui64
#include <cstring>   // memcpy

#pragma comment(lib, "bcrypt.lib")

namespace
{

constexpr wchar_t kFileName[] = L"moonlight_sideband.ini";

// 生成 32 个十六进制字符（128 bit）的随机令牌
std::string GenerateToken()
{
    unsigned char raw[16] = {};

    // BCRYPT_USE_SYSTEM_PREFERRED_RNG 允许传 nullptr 算法句柄，
    // 返回 STATUS_SUCCESS(0) 表示成功
    NTSTATUS st = BCryptGenRandom(nullptr, raw, (ULONG)sizeof(raw),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0)
    {
        // 退化路径：拿几个高熵计数器凑，聊胜于无并明确记日志
        Logger::Get().Error("Config: BCryptGenRandom 失败(", (long)st,
                            ")，使用低强度回退令牌，建议手动改 ini 里的 token");
        LARGE_INTEGER qpc = {};
        QueryPerformanceCounter(&qpc);
        uint64_t a = (uint64_t)qpc.QuadPart;
        uint64_t b = (uint64_t)GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 32);
        memcpy(raw, &a, 8);
        memcpy(raw + 8, &b, 8);
    }

    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(raw) * 2);
    for (unsigned char c : raw)
    {
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
    }
    return out;
}

std::string Trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ')
        ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ')
        --e;
    return s.substr(b, e - b);
}

bool ParseBool(const std::string &v, bool defaultValue)
{
    std::string l = v;
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (l == "1" || l == "true" || l == "yes" || l == "on")
        return true;
    if (l == "0" || l == "false" || l == "no" || l == "off")
        return false;
    return defaultValue;
}

} // namespace

std::wstring Config::FilePath()
{
    std::wstring dir = Win32Util::GetExeDirectory();
    if (dir.empty())
        return kFileName;  // 退化到 CWD
    return dir + kFileName;
}

bool Config::LoadOrCreate()
{
    const std::wstring path = FilePath();
    bool needSave = false;

    std::ifstream in(path.c_str());
    if (!in.is_open())
    {
        // 首次运行
        token = GenerateToken();
        needSave = true;
    }
    else
    {
        std::string line;
        while (std::getline(in, line))
        {
            std::string s = Trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';')
                continue;

            size_t eq = s.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = Trim(s.substr(0, eq));
            std::string val = Trim(s.substr(eq + 1));
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });

            if (key == "port")
            {
                int p = atoi(val.c_str());
                if (p > 0 && p <= 65535)
                    port = (uint16_t)p;
            }
            else if (key == "loopback_only")
            {
                loopbackOnly = ParseBool(val, loopbackOnly);
            }
            else if (key == "log_level")
            {
                std::transform(val.begin(), val.end(), val.begin(),
                               [](unsigned char c) { return (char)std::toupper(c); });
                if (val == "TRACE" || val == "DEBUG" || val == "INFO" || val == "ERROR")
                    logLevel = val;
            }
            else if (key == "log_max_bytes")
            {
                logMaxBytes = (uint64_t)_strtoui64(val.c_str(), nullptr, 10);
            }
            else if (key == "token")
            {
                token = val;
            }
        }
        in.close();

        if (token.empty())
        {
            // 用户删掉了 token 行 —— 重新生成
            token = GenerateToken();
            needSave = true;
        }
    }

    if (needSave)
        return Save();
    return true;
}

bool Config::Save() const
{
    const std::wstring path = FilePath();
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out.is_open())
        return false;

    out << "# moonlight-sideband 配置文件（UTF-8）\n"
        << "# 修改后需重启程序生效。\n"
        << "\n"
        << "# 监听端口\n"
        << "port=" << port << "\n"
        << "\n"
        << "# 是否只监听本机回环地址。\n"
        << "# true  = 只有本机能连（手机需走 SSH / adb 端口转发），最安全\n"
        << "# false = 局域网可直连；此时改系统状态的指令由下面的 token 保护\n"
        << "loopback_only=" << (loopbackOnly ? "true" : "false") << "\n"
        << "\n"
        << "# 日志级别：TRACE / DEBUG / INFO / ERROR\n"
        << "log_level=" << logLevel << "\n"
        << "\n"
        << "# 单个日志文件字节上限，超出后轮转为 .1（0 = 不限制）\n"
        << "log_max_bytes=" << logMaxBytes << "\n"
        << "\n"
        << "# 控制令牌：切换显示器 / 设置分辨率 / 设置缩放 需要客户端先提交它。\n"
        << "# 光标推送和只读查询不需要，所以老客户端不受影响。\n"
        << "# 请把这串值填到 Android 端；删掉本行会在下次启动时重新生成。\n"
        << "token=" << token << "\n";

    return out.good();
}
