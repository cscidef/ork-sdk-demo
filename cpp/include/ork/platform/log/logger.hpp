#pragma once
#include "ork/platform/log/log_level.hpp"
#include <fmt/format.h>
#include <memory>
#include <source_location>
#include <string>

class Logger
{
public:
    static Logger& Instance();

    void Init(const std::string& name,
              const std::string& logFile,
              LogLevel level);

    /// 轻量级 level 检查（无 IO），用于延迟格式化路径。
    bool ShouldLog(LogLevel level) const noexcept { return level >= level_; }

    // ── 延迟格式化模板入口（热路径优先使用） ──
    // 先判 level，通过才 format，避免无效字符串构造开销。

    template<typename... Args>
    void Debug(fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (ShouldLog(LogLevel::Debug))
            LogDirect(LogLevel::Debug, fmt::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void Info(fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (ShouldLog(LogLevel::Info))
            LogDirect(LogLevel::Info, fmt::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void Warn(fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (ShouldLog(LogLevel::Warn))
            LogDirect(LogLevel::Warn, fmt::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void Error(fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (ShouldLog(LogLevel::Error))
            LogDirect(LogLevel::Error, fmt::format(fmt, std::forward<Args>(args)...));
    }

    // ── string overload（兼容非热路径 / 启动停止日志） ──

    void Trace(const std::string& msg, const std::source_location& loc = std::source_location::current());
    void Debug(const std::string& msg, const std::source_location& loc = std::source_location::current());
    void Info(const std::string& msg, const std::source_location& loc = std::source_location::current());
    void Warn(const std::string& msg, const std::source_location& loc = std::source_location::current());
    void Error(const std::string& msg, const std::source_location& loc = std::source_location::current());
    void Critical(const std::string& msg, const std::source_location& loc = std::source_location::current());

private:
    Logger() = default;

    void Log(LogLevel level, const std::string& msg, const std::source_location& loc);
    void LogDirect(LogLevel level, const std::string& msg);

private:
    std::shared_ptr<void> logger_;
    LogLevel level_{LogLevel::Info};
};
