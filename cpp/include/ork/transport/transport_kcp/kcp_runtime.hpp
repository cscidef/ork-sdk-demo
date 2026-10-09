/**
 * @file kcp_runtime.hpp
 * @brief KCP transport 层头：引入 platform KcpConfig + MonotonicMs32 + KcpDiagnostics
 *
 * Phase 4.1-fix：KcpConfig 值类型已迁移至 ork/platform/data/kcp_config.hpp。
 * 本文件为 transport 层运行时聚合入口（时钟 + 诊断 + 值类型转发）。
 *
 * 内容：
 * - KcpConfig（值结构 + Validate）→ 来自 platform/data/kcp_config.hpp
 * - MonotonicMs32()：transport 层唯一时钟源（依赖 chrono，不涉及 ikcp）
 * - KcpDiagnostics：只读诊断采样（rtoRetransmits 仅含 RTO 超时重传）
 */

#pragma once

#include "ork/platform/data/kcp_config.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace ork::transport {

// =============================================================================
// MonotonicMs32 — transport 层唯一时钟
// =============================================================================

/**
 * @brief 返回基于 std::chrono::steady_clock 的毫秒值，截断为 uint32_t（KCP IUINT32 环绕语义）。
 *
 * 所有 KcpSession::Update 调用点（pump 循环、UDP Input 后即时 Update）必须使用本函数，
 * 不再接受 GameClock / 外部任意 uint64_t 时钟。
 *
 * KCP 内部使用 IUINT32（32 位无符号）表示时间戳，约 49.7 天自然环绕；
 * steady_clock 保证单调递增，截断到 32 位后环绕行为与 KCP 预期一致。
 */
inline uint32_t MonotonicMs32() noexcept
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return static_cast<uint32_t>(ms);
}

// =============================================================================
// KcpDiagnostics — 只读诊断采样
// =============================================================================

/**
 * @brief KCP 只读诊断采样
 *
 * 提供 KCP 内部关键运行时指标，供监控/调试使用。
 * 不暴露 ikcpcb 裸指针，不改变 C ABI。
 */
struct KcpDiagnostics
{
    /// 等待发送的段数（ikcp_waitsnd）
    int waitsnd = 0;
    /// 平滑 RTT（ms），来自 ikcpcb::rx_srtt
    int32_t rx_srtt = 0;
    /// 当前 RTO（ms），来自 ikcpcb::rx_rto
    int32_t rx_rto = 0;
    /**
     * RTO 超时重传累计次数（ikcpcb::xmit）。
     *
     * 注意：ikcp.c 仅在 RTO timeout retransmit 分支递增此计数器，
     * **不含**首次发送（segment->xmit==0 分支）与 fast retransmit（fastack 分支）。
     * 若需完整重传统计，应在应用层通过 IKCPOPS 回调或差值采样实现。
     */
    uint32_t rtoRetransmits = 0;
};

}  // namespace ork::transport
