#pragma once

#include <mutex>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <ctime>
#include <cstdlib>

enum class LogLevel
{
    TRACE,
    DEBUG,
    INFO,
    LOG_ERROR
};

class Logger
{
    std::mutex m_mutex;
    std::ofstream m_file;
    LogLevel m_level;

public:
    static Logger &Get()
    {
        static Logger instance;
        return instance;
    }

    Logger() : m_level(LogLevel::INFO)
    {
        m_file.open("moonlight_sideband.log", std::ios::app);
        char *envLevel = nullptr;
        size_t len = 0;
        _dupenv_s(&envLevel, &len, "SIDEBAND_LOG_LEVEL");
        if (envLevel)
        {
            std::string l(envLevel);
            if (l == "TRACE")
                m_level = LogLevel::TRACE;
            else if (l == "DEBUG")
                m_level = LogLevel::DEBUG;
            else if (l == "INFO")
                m_level = LogLevel::INFO;
            else if (l == "ERROR")
                m_level = LogLevel::LOG_ERROR;
            free(envLevel);
        }
    }

    ~Logger()
    {
        if (m_file.is_open())
            m_file.close();
    }

    void SetLogLevel(LogLevel l) { m_level = l; }

    template <typename... Args>
    void Info(Args... args)
    {
        if (m_level <= LogLevel::INFO)
            Log("[INFO] ", args...);
    }
    template <typename... Args>
    void Error(Args... args)
    {
        if (m_level <= LogLevel::LOG_ERROR)
            Log("[ERROR] ", args...);
    }
    template <typename... Args>
    void Warning(Args... args)
    {
        if (m_level <= LogLevel::INFO)
            Log("[WARN] ", args...);
    }
    template <typename... Args>
    void Debug(Args... args)
    {
        if (m_level <= LogLevel::DEBUG)
            Log("[DEBUG] ", args...);
    }
    template <typename... Args>
    void Trace(Args... args)
    {
        if (m_level <= LogLevel::TRACE)
            Log("[TRACE] ", args...);
    }

private:
    template <typename... Args>
    void Log(const char *prefix, Args... args)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_now;
        localtime_s(&tm_now, &now);

        std::cout << std::put_time(&tm_now, "%H:%M:%S ") << prefix;
        ((std::cout << args << " "), ...);
        std::cout << std::endl;

        if (m_file.is_open())
        {
            m_file << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S ") << prefix;
            ((m_file << args << " "), ...);
            m_file << std::endl;
        }
    }
};
