#pragma once

/**
 * 配置文件（moonlight_sideband.ini，固定放在 exe 同目录）
 *
 * 之所以需要它：进程是 /SUBSYSTEM:WINDOWS 的，没有控制台，
 * 命令行参数既看不到帮助也不方便从任务计划里改，
 * 所以端口、日志级别、令牌都落到文件里。
 * 首次运行会自动生成一份带注释的默认配置。
 */

#include <string>
#include <cstdint>

struct Config
{
    uint16_t port = 5005;

    // 只监听 127.0.0.1 / ::1。开启后手机无法直连，需要 SSH/adb 端口转发。
    bool loopbackOnly = false;

    // TRACE / DEBUG / INFO / ERROR
    std::string logLevel = "INFO";

    // 单个日志文件上限，超过后轮转为 .1（0 表示不限制）
    uint64_t logMaxBytes = 5ull * 1024 * 1024;

    // 控制指令令牌。改变系统状态的指令（切换显示器 / 设分辨率 / 设缩放）
    // 要求客户端先用 AUTH_REQ 提交它。首次运行随机生成。
    std::string token;

    // 从 exe 同目录的 ini 载入；文件不存在或缺字段时补齐并回写。
    // 返回 false 表示配置文件读写失败（此时使用内存中的默认值继续运行）。
    bool LoadOrCreate();

    // 配置文件完整路径
    static std::wstring FilePath();

private:
    bool Save() const;
};
