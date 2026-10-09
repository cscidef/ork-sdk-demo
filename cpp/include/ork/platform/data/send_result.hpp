/**
 * @file send_result.hpp
 * @brief SendResult 结构化发送结果枚举 + ToString（platform 层）
 *
 * Phase 4.2：从 ork/sdk/result.hpp 下沉至 platform 层，
 * 使 transport 层（NetworkTransport::TrySend）可直接返回结构化结果，
 * 而不引入 transport → sdk 的反向依赖。
 *
 * ork/sdk/result.hpp 通过 using 别名将其重新导出到 ork::sdk 命名空间，
 * 保持既有 SDK 公共 API 语义不变。
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace ork::platform {

/// 单次发送结果
enum class SendResult : uint8_t {
    Ok              = 0,  ///< 发送成功（已入传输层队列）
    InvalidArg      = 1,  ///< 空指针 / 零长度
    NotConnected    = 2,  ///< 目标连接不存在或已断开
    NoRoute         = 3,  ///< 无可用路由（UDP endpoint 未绑定等）
    TooLarge        = 4,  ///< payload 超过 maxPacketSize
    QueueFull       = 5,  ///< 传输层发送队列满
    CipherFailed    = 6,  ///< 加密失败（AEAD Seal 返回 nullopt）
    BadState        = 7,  ///< 其他 / 未知底层失败
};

/// 人类可读字符串（静态，不分配内存）
constexpr std::string_view ToString(SendResult r) noexcept
{
    switch (r)
    {
    case SendResult::Ok:           return "Ok";
    case SendResult::InvalidArg:   return "InvalidArg";
    case SendResult::NotConnected: return "NotConnected";
    case SendResult::NoRoute:      return "NoRoute";
    case SendResult::TooLarge:     return "TooLarge";
    case SendResult::QueueFull:    return "QueueFull";
    case SendResult::CipherFailed: return "CipherFailed";
    case SendResult::BadState:     return "BadState";
    }
    return "Unknown";
}

}  // namespace ork::platform
