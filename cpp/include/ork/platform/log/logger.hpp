#pragma once
#include "ork/platform/log/log_level.hpp"
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>

// ── source_location 兼容层 ──
// Android NDK r23b 的 libc++（LLVM 12）尚未提供 <source_location>（该头
// 自 libc++ 16 才完整实现）。此处按能力探测：有则直接用标准类型，没有
// 则退化为零开销占位类型，日志照常输出、仅不再携带文件/行号。
// 该降级不影响 C ABI 与传输语义，仅影响日志定位信息。
#if defined(__has_include)
#  if __has_include(<source_location>)
#    include <source_location>
#    define ORK_HAS_STD_SOURCE_LOCATION 1
#  endif
#endif
#ifndef ORK_HAS_STD_SOURCE_LOCATION
#  define ORK_HAS_STD_SOURCE_LOCATION 0
#endif

#if ORK_HAS_STD_SOURCE_LOCATION
using OrkSourceLocation = std::source_location;
#else
class OrkSourceLocation
{
public:
    static constexpr OrkSourceLocation current() noexcept { return {}; }
    constexpr const char* file_name() const noexcept { return ""; }
    constexpr const char* function_name() const noexcept { return ""; }
    constexpr std::uint_least32_t line() const noexcept { return 0; }
    constexpr std::uint_least32_t column() const noexcept { return 0; }
};
#endif

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

    void Trace(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());
    void Debug(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());
    void Info(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());
    void Warn(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());
    void Error(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());
    void Critical(const std::string& msg, const OrkSourceLocation& loc = OrkSourceLocation::current());

private:
    Logger() = default;

    void Log(LogLevel level, const std::string& msg, const OrkSourceLocation& loc);
    void LogDirect(LogLevel level, const std::string& msg);

private:
    std::shared_ptr<void> logger_;
    LogLevel level_{LogLevel::Info};
};
