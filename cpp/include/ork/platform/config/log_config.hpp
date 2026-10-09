#pragma once
#include "ork/platform/log/log_level.hpp"
#include <stdexcept>
#include <string>

struct LogConfig
{
    // Phase 4.5：默认成员初始化器——防御 includes 未列出 log.yaml 时读未初始化字段。
    LogLevel level{LogLevel::Info};
    std::string file{"logs/ork.log"};

    void Validate() const
    {
        if (level < LogLevel::Trace || level > LogLevel::Off)
            throw std::runtime_error("LogConfig: invalid log level");
    }
};
