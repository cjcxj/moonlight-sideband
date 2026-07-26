#pragma once

/**
 * Win32 小工具集合
 *
 * 这些函数以前是 DisplayModule.hpp 里的**全局命名空间自由函数**
 * （WideToUtf8 / EscapeJson / ParseJsonStringField / ParseJsonIntField），
 * 容易与其他 TU 撞名。现在字符串相关的收进 Win32Util 命名空间，
 * JSON 相关的搬到 include/Json.hpp。
 */

#include <string>

namespace Win32Util
{

// 宽字符 -> UTF-8
std::string WideToUtf8(const std::wstring &w);

// UTF-8 -> 宽字符
std::wstring Utf8ToWide(const std::string &s);

// 当前 exe 所在目录（结尾带反斜杠）。失败时返回空串。
// 用于把日志/配置文件固定放在 exe 旁边，而不是随 CWD 漂移
//（从任务计划启动时 CWD 往往是 C:\Windows\System32）。
std::wstring GetExeDirectory();

// 当前 exe 完整路径
std::wstring GetExePath();

} // namespace Win32Util
