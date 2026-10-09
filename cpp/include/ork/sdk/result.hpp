/**
 * @file result.hpp
 * @brief SendResult 结构化发送结果枚举 + ToString
 *
 * Send() 返回单个 SendResult；Broadcast() 返回 BroadcastReport（见 sender.hpp）。
 *
 * Phase 4.2：SendResult 定义已下沉至 ork/platform/data/send_result.hpp
 * （供 transport 层直接返回，避免 transport → sdk 反向依赖）。
 * 本头文件通过 using 别名将其重新导出到 ork::sdk 命名空间，
 * 既有 SDK 公共 API（ork::sdk::SendResult / ork::sdk::ToString）语义不变。
 */

#pragma once

#include "ork/platform/data/send_result.hpp"

#include <cstdint>
#include <string_view>

namespace ork::sdk {

/// 单次发送结果（重导出自 ork::platform::SendResult）
using SendResult = ork::platform::SendResult;

/// 人类可读字符串（重导出自 ork::platform）
using ork::platform::ToString;

}  // namespace ork::sdk
