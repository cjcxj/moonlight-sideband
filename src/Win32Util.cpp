#include "Win32Util.hpp"

#include <windows.h>
#include <vector>

namespace Win32Util
{

std::string WideToUtf8(const std::wstring &w)
{
    if (w.empty())
        return std::string();

    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return std::string();

    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string &s)
{
    if (s.empty())
        return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0)
        return std::wstring();

    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::wstring GetExePath()
{
    // GetModuleFileNameW 在缓冲区不足时会截断且（XP 之后）不保证置零终止，
    // 这里循环扩容直到确认没有截断
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0)
            return std::wstring();
        if (n < buf.size() - 1)
            return std::wstring(buf.data(), n);
        if (buf.size() >= 32768)
            return std::wstring();
        buf.resize(buf.size() * 2);
    }
}

std::wstring GetExeDirectory()
{
    std::wstring path = GetExePath();
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return std::wstring();
    return path.substr(0, pos + 1);
}

} // namespace Win32Util
