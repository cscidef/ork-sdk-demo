/**
 * @file channel.hpp
 * @brief 公共 SDK 通道枚举，与 TransportChannel 数值对齐
 *
 * 业务代码使用 ork::sdk::Channel；内部传输层使用 TransportChannel。
 * static_assert 保证两者数值一致，提供零成本安全转换。
 */

#pragma once

#include "ork/platform/data/transport_channel.hpp"
#include <cstdint>
#include <type_traits>

namespace ork::sdk {

/// 公共通道枚举（对外 API）
enum class Channel : uint8_t {
    Reliable   = 0,  ///< 可靠通道（KCP over UDP）
    Unreliable = 1   ///< 不可靠通道（UDP）
};

// ===== 数值对齐断言 =====
static_assert(static_cast<uint8_t>(Channel::Reliable)   == static_cast<uint8_t>(TransportChannel::Reliable),
              "Channel::Reliable must equal TransportChannel::Reliable");
static_assert(static_cast<uint8_t>(Channel::Unreliable) == static_cast<uint8_t>(TransportChannel::Unreliable),
              "Channel::Unreliable must equal TransportChannel::Unreliable");

/// Channel → TransportChannel（零成本）
constexpr TransportChannel ToTransportChannel(Channel ch) noexcept
{
    return static_cast<TransportChannel>(static_cast<uint8_t>(ch));
}

/// TransportChannel → Channel（零成本）
constexpr Channel FromTransportChannel(TransportChannel ch) noexcept
{
    return static_cast<Channel>(static_cast<uint8_t>(ch));
}

} // namespace ork::sdk
