#pragma once

/**
 * 简易日志
 *
 * 相比早期版本的改动：
 * 1. 日志文件固定落在 exe 同目录，不再跟着 CWD 走
 *    （从任务计划启动时 CWD 是 C:\Windows\System32，日志会写进系统目录）
 * 2. 按大小轮转（xxx.log -> xxx.log.1），不再无限增长
 * 3. 去掉 std::cout —— 进程是 /SUBSYSTEM:WINDOWS，没有控制台，
 *    往 cout 写是纯浪费；改成 DEBUG 级别时同步一份到调试器输出
 * 4. 去掉 MSVC 专有的 _dupenv_s（日志级别改由配置文件提供）
 */

#include <windows.h>

#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cstdint>
#include <utility>

#include "Win32Util.hpp"

enum class LogLevel
{
    TRACE,
    DEBUG,
    INFO,
    LOG_ERROR
};

class Logger
{
public:
    static Logger &Get()
    {
        static Logger instance;
        return instance;
    }

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // 由 main 在读完配置后调用。未调用时使用默认值（exe 目录 / INFO / 5MB）。
    void Init(LogLevel level, uint64_t maxBytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_level.store(level, std::memory_order_relaxed);
        m_maxBytes = maxBytes;
    }

    void SetLogLevel(LogLevel l) { m_level.store(l, std::memory_order_relaxed); }

    LogLevel GetLogLevel() const { return m_level.load(std::memory_order_relaxed); }

    static bool ParseLevel(const std::string &s, LogLevel &out)
    {
        if (s == "TRACE") { out = LogLevel::TRACE;     return true; }
        if (s == "DEBUG") { out = LogLevel::DEBUG;     return true; }
        if (s == "INFO")  { out = LogLevel::INFO;      return true; }
        if (s == "ERROR") { out = LogLevel::LOG_ERROR; return true; }
        return false;
    }

    template <typename... Args>
    void Info(Args &&...args)
    {
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::INFO)
            Log("[INFO] ", std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Error(Args &&...args)
    {
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::LOG_ERROR)
            Log("[ERROR] ", std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warning(Args &&...args)
    {
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::INFO)
            Log("[WARN] ", std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Debug(Args &&...args)
    {
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::DEBUG)
            Log("[DEBUG] ", std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Trace(Args &&...args)
    {
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::TRACE)
            Log("[TRACE] ", std::forward<Args>(args)...);
    }

private:
    Logger() : m_level(LogLevel::INFO) {}

    ~Logger()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open())
            m_file.close();
    }

    template <typename... Args>
    void Log(const char *prefix, Args &&...args)
    {
        // 先在锁外把内容拼好：缩短临界区，也便于知道本条长度用于轮转判断
        std::ostringstream ss;
        ss << TimeStamp() << prefix;
        ((ss << args << " "), ...);
        ss << "\n";
        const std::string line = ss.str();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RotateIfNeededLocked(line.size());
            EnsureOpenLocked();

            if (m_file.is_open())
            {
                m_file << line;
                m_file.flush();
                m_size += line.size();
            }
        }

        // DEBUG 及以下级别时同步一份给调试器（附加 VS / DebugView 可见）
        if (m_level.load(std::memory_order_relaxed) <= LogLevel::DEBUG)
            OutputDebugStringA(line.c_str());
    }

    static std::string TimeStamp()
    {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tmNow = {};
#if defined(_MSC_VER)
        localtime_s(&tmNow, &now);
#else
        localtime_r(&now, &tmNow);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tmNow, "%Y-%m-%d %H:%M:%S ");
        return ss.str();
    }

    static std::wstring LogPath()
    {
        return Win32Util::GetExeDirectory() + L"moonlight_sideband.log";
    }

    void EnsureOpenLocked()
    {
        if (m_file.is_open())
            return;

        m_file.open(LogPath().c_str(), std::ios::app | std::ios::binary);
        if (m_file.is_open())
        {
            m_file.seekp(0, std::ios::end);
            std::streampos pos = m_file.tellp();
            m_size = (pos > 0) ? (uint64_t)pos : 0;
        }
    }

    void RotateIfNeededLocked(size_t incoming)
    {
        if (m_maxBytes == 0)
            return;

        // 尚未打开时先探一次当前大小
        if (!m_file.is_open())
            EnsureOpenLocked();

        if (m_size + incoming <= m_maxBytes)
            return;

        if (m_file.is_open())
            m_file.close();

        const std::wstring path = LogPath();
        const std::wstring backup = path + L".1";
        DeleteFileW(backup.c_str());
        MoveFileW(path.c_str(), backup.c_str());

        m_size = 0;
        // 下次 EnsureOpenLocked 会新建文件
    }

    std::mutex m_mutex;
    std::ofstream m_file;
    std::atomic<LogLevel> m_level;
    uint64_t m_maxBytes = 5ull * 1024 * 1024;
    uint64_t m_size = 0;
};
