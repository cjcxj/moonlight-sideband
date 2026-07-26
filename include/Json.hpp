#pragma once

/**
 * 极简 JSON 读写辅助（纯 C++，不依赖 Windows，可单元测试）
 *
 * 说明：本项目的 JSON 只用于几个扁平对象的字段读写，不引入完整解析器。
 * 但与之前内联在 DisplayModule 里的版本相比，这里的查找是"带状态扫描"的：
 * 它会正确跳过字符串字面量内部的内容，因此不会再把出现在 **值** 里的
 * `"display_id"` 之类文本误判成键。例如：
 *
 *     {"msg":"display_id: not found","display_id":"\\\\.\\DISPLAY1"}
 *
 * 旧实现会命中前一个（值内部的），本实现命中后一个（真正的键）。
 */

#include <string>
#include <cstdint>
#include <cstdio>
#include <climits>

namespace Json
{

// 转义字符串以便嵌入 JSON 字面量
inline std::string Escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

namespace detail
{

inline bool IsWs(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 从 pos（指向起始引号）开始解析一个 JSON 字符串字面量。
// 成功时 outValue 为反转义后的内容，outEnd 指向结束引号之后的位置。
inline bool ScanString(const std::string &json, size_t pos,
                       std::string &outValue, size_t &outEnd)
{
    const size_t n = json.size();
    if (pos >= n || json[pos] != '"')
        return false;

    outValue.clear();
    size_t i = pos + 1;
    while (i < n)
    {
        char c = json[i];
        if (c == '\\')
        {
            if (i + 1 >= n)
                return false;  // 反斜杠后截断
            char e = json[i + 1];
            switch (e)
            {
            case '"':  outValue += '"';  break;
            case '\\': outValue += '\\'; break;
            case '/':  outValue += '/';  break;
            case 'n':  outValue += '\n'; break;
            case 't':  outValue += '\t'; break;
            case 'r':  outValue += '\r'; break;
            case 'b':  outValue += '\b'; break;
            case 'f':  outValue += '\f'; break;
            case 'u':
            {
                // \uXXXX：本项目不需要完整 UTF-16 解码，
                // 仅消费 4 位十六进制并对 ASCII 范围做还原，其余用 '?' 占位
                if (i + 5 >= n)
                    return false;
                unsigned code = 0;
                bool ok = true;
                for (int k = 0; k < 4; ++k)
                {
                    char h = json[i + 2 + k];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    else { ok = false; break; }
                }
                if (!ok)
                    return false;
                outValue += (code < 0x80) ? (char)code : '?';
                i += 6;
                continue;
            }
            default:
                outValue += e;  // 未知转义：保留原字符
                break;
            }
            i += 2;
            continue;
        }
        if (c == '"')
        {
            outEnd = i + 1;
            return true;
        }
        outValue += c;
        ++i;
    }
    return false;  // 未闭合
}

// 定位键 key 对应值的起始位置（跳过所有字符串字面量内部内容）。
// 返回 npos 表示未找到。
inline size_t FindValuePos(const std::string &json, const std::string &key)
{
    const size_t n = json.size();
    size_t i = 0;
    while (i < n)
    {
        if (json[i] != '"')
        {
            ++i;
            continue;
        }

        std::string token;
        size_t end = 0;
        if (!ScanString(json, i, token, end))
            return std::string::npos;  // 畸形 JSON，放弃

        // 结束引号之后若（跳过空白后）是冒号，说明这是一个键
        size_t k = end;
        while (k < n && IsWs(json[k]))
            ++k;

        if (k < n && json[k] == ':')
        {
            if (token == key)
            {
                size_t v = k + 1;
                while (v < n && IsWs(json[v]))
                    ++v;
                return v;
            }
            i = k + 1;   // 从值处继续扫描
        }
        else
        {
            i = end;     // 这是个值（或数组元素），跳过它
        }
    }
    return std::string::npos;
}

} // namespace detail

// 读取字符串字段。未找到或类型不符时返回 defaultValue。
inline std::string GetString(const std::string &json, const std::string &key,
                             const std::string &defaultValue = std::string())
{
    size_t v = detail::FindValuePos(json, key);
    if (v == std::string::npos || v >= json.size() || json[v] != '"')
        return defaultValue;

    std::string out;
    size_t end = 0;
    if (!detail::ScanString(json, v, out, end))
        return defaultValue;
    return out;
}

// 读取整数字段。未找到、非数字或溢出时返回 defaultValue；
// found 非空时回写"是否成功读到一个合法整数"。
inline int GetInt(const std::string &json, const std::string &key,
                  int defaultValue = 0, bool *found = nullptr)
{
    if (found)
        *found = false;

    size_t i = detail::FindValuePos(json, key);
    if (i == std::string::npos || i >= json.size())
        return defaultValue;

    bool negative = false;
    if (json[i] == '-' || json[i] == '+')
    {
        negative = (json[i] == '-');
        ++i;
    }

    if (i >= json.size() || json[i] < '0' || json[i] > '9')
        return defaultValue;

    // 用 int64 累加并做上下限钳制，避免 int 溢出成 UB
    long long value = 0;
    bool overflow = false;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9')
    {
        value = value * 10 + (json[i] - '0');
        if (value > 4294967296LL)  // 远超 int 范围即可判定溢出
        {
            overflow = true;
            break;
        }
        ++i;
    }
    if (overflow)
        return defaultValue;

    if (negative)
        value = -value;
    if (value > INT_MAX || value < INT_MIN)
        return defaultValue;

    if (found)
        *found = true;
    return (int)value;
}

// 读取布尔字段。
inline bool GetBool(const std::string &json, const std::string &key,
                    bool defaultValue = false)
{
    size_t v = detail::FindValuePos(json, key);
    if (v == std::string::npos)
        return defaultValue;
    if (json.compare(v, 4, "true") == 0)
        return true;
    if (json.compare(v, 5, "false") == 0)
        return false;
    return defaultValue;
}

} // namespace Json
