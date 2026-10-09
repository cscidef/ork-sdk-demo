/**
 * @file kcp_config.hpp
 * @brief KCP 协议参数配置值类型（platform 层，无 transport/ikcp 依赖）
 *
 * Phase 4.1 引入，Phase 4.1-fix 从 transport 层迁移至 platform 层。
 * 仅含纯值结构与校验逻辑；时钟（MonotonicMs32）与诊断（KcpDiagnostics）
 * 保留在 transport 层（ork/transport/transport_kcp/kcp_runtime.hpp）。
 *
 * 依赖方向：platform ← transport ← sdk（正确，platform 不反向依赖 transport）。
 */

#pragma once

#include <stdexcept>
#include <string>

namespace ork::transport {

// =============================================================================
// MTU 合法性常量（基于 ikcp.c 实现 + OrkCipherFrame + IPv6/UDP 链路预算）
// =============================================================================

/**
 * @brief ikcp_setmtu 硬下限（字节）
 *
 * ikcp.c L1318: `if (mtu < 50 || mtu < (int)IKCP_OVERHEAD) return -1;`
 * IKCP_OVERHEAD = 24，故绑定下限为 50。
 */
inline constexpr int kKcpMtuMin = 50;

/**
 * @brief 共享包长硬上限（字节）—— 生产校验常量
 *
 * NetworkConfig::max_packet_size 同时作为 ikcp_setmtu 的 MTU 参数和
 * 应用 payload 上限（Reliable 与 Unreliable 两通道共享同一个值），
 * 因此必须取两通道无 IPv6 分片边界中更严格者：
 *
 * ── Reliable（KCP）通道 ──
 *
 *   ikcp_setmtu(mtu) 设定 kcp->mtu = mtu，kcp->mss = mtu − IKCP_OVERHEAD(24)。
 *   KCP flush 条件：accumulated + IKCP_OVERHEAD + seg->len > mtu → 输出；
 *   因此 output callback 收到的 buffer 始终 ≤ mtu 字节。
 *   OrkCipherFrame（kFrameHeaderSize=15 + AEAD tag 16 = 31B 开销）是 KCP 的
 *   用户数据，被 ikcp_send 按 MSS 分片，不额外增加 wire 长度。
 *   注意：mtu 本身已是 output 上限（含 24B KCP header），预算中不得再减
 *   IKCP_OVERHEAD（旧值 1421 即因双算而偏保守）。
 *
 *   wire_datagram = IPv6(40) + UDP(8) + kcp_output_size
 *                 = 48 + kcp_output_size ≤ 48 + mtu
 *   无分片约束：48 + mtu ≤ 1500 → mtu ≤ 1452（仅本通道的理论上限，
 *   见 kKcpReliableMtuMax，不得作为共享配置上限）。
 *
 * ── Unreliable（Raw UDP）通道 ──
 *
 *   wire_datagram = IPv6(40) + UDP(8) + UdpDataHeader(7)
 *                 + OrkCipherFrame(kFrameHeaderSize 15 + AEAD tag 16 + payload)
 *                 = 86 + app_payload（加密最坏情况）
 *   无分片约束：app_payload ≤ 1500 − 86 = 1414。
 *   未加密仅少 16B AEAD tag（上限 1430），但共享上限按加密最坏情况校验。
 *
 * ── 共享上限（生产校验值）──
 *
 *   同一个 max_packet_size 既传 ikcp_setmtu 又限制应用 payload：
 *     min(1452, 1414) = 1414
 *
 *   即 [1415, 1452] 区间对 Reliable 单通道虽然合法，但对共享配置一律拒绝
 *   （会令 Unreliable 加密最坏 wire 达 1501..1538，触发 IPv6 分片）。
 *
 * kKcpMtuMax = 1414（精确上限，无人为 margin）；默认 1200 不受影响。
 */
inline constexpr int kKcpMtuMax = 1414;

/**
 * @brief Reliable（KCP）单通道理论 MTU 上限（字节）—— 仅作推导说明，
 *        不得作为共享配置 max_packet_size 的上限
 *
 *   48(IPv6+UDP) + mtu ≤ 1500 → mtu ≤ 1500 − 48 = 1452
 *
 * 共享配置实际使用更严格的 kKcpMtuMax(1414)；本常量仅用于文档、
 * static_assert 与单测中表述 Reliable 通道独立边界。
 */
inline constexpr int kKcpReliableMtuMax = 1500 - 48;  // 1452

// ── 编译期常量校验 ────────────────────────────────────────────────
static_assert(kKcpMtuMin == 50, "kKcpMtuMin must match ikcp.c hard minimum");
static_assert(kKcpMtuMax == 1500 - 48 - 7 - 15 - 16,
              "kKcpMtuMax: shared cap = 1500 - 48(IPv6+UDP) - 7(UdpDataHeader)"
              " - 15(OrkFrame header) - 16(AEAD tag) = 1414");
static_assert(kKcpReliableMtuMax == 1500 - 48,
              "kKcpReliableMtuMax: 48(IPv6+UDP) + mtu <= 1500 -> 1452");
static_assert(kKcpMtuMax < kKcpReliableMtuMax,
              "shared cap must be stricter than Reliable-only limit"
              " (delta = 7+15+16 = 38)");
static_assert(kKcpMtuMax == kKcpReliableMtuMax - 7 - 15 - 16,
              "shared cap = Reliable limit minus UdpDataHeader/OrkFrame/AEAD");
static_assert(kKcpMtuMin < kKcpMtuMax, "kKcpMtuMin must be < kKcpMtuMax");
static_assert(kKcpMtuMax >= 1200, "kKcpMtuMax must accommodate default 1200");

/**
 * @brief 默认共享包长（字节）—— 安全默认值，分散引用的唯一权威常量
 *
 * 嵌入用户未走 YAML 时不会回退到 65535；生产路径由 NetworkConfig 填充。
 */
inline constexpr int kDefaultMaxPacketSize = 1200;
static_assert(kDefaultMaxPacketSize >= kKcpMtuMin && kDefaultMaxPacketSize <= kKcpMtuMax,
              "kDefaultMaxPacketSize must be within [kKcpMtuMin, kKcpMtuMax]");

// =============================================================================
// KcpConfig — KCP 协议参数
// =============================================================================

/**
 * @brief KCP 协议参数值结构
 *
 * 默认值严格等于 Phase 2 硬编码（kcp_session.cpp 中 ikcp_nodelay / ikcp_wndsize 等调用）。
 * MTU 不在此结构存储；权威来源为 NetworkConfig::max_packet_size，
 * 由 NetworkTransport 在创建 KcpSession 时传入。
 */
struct KcpConfig
{
    /// nodelay 模式：0=关闭，1=无延迟，2=最快（RTO x1.5 退避）。默认 2。
    int nodelay = 2;
    /// KCP 内部 flush 间隔（ms）。默认 10。
    int interval_ms = 10;
    /// 快速重传阈值（跳过 N 个 ACK 即重传）。默认 2。
    int fast_resend = 2;
    /// true=关闭拥塞控制（游戏标准做法）。默认 true。
    bool no_congestion = true;
    /// 发送窗口大小（段数）。默认 128。
    int send_window = 128;
    /// 接收窗口大小（段数）。默认 128。
    int receive_window = 128;
    /// true=流模式（每次 Send 不一定对应一次 Recv）。默认 false（消息模式）。
    bool stream_mode = false;
    /// 最小 RTO（ms）。默认 10（局域网 RTT<5ms 场景）。
    int rx_min_rto_ms = 10;

    /**
     * @brief 校验 KcpConfig 参数合法性
     *
     * 校验规则：
     * - nodelay ∈ {0, 1, 2}
     * - interval_ms ∈ [10, 5000]
     * - fast_resend ∈ [0, 128]
     * - send_window ∈ [1, 8192]
     * - receive_window ∈ [1, 8192]
     * - rx_min_rto_ms ∈ [1, 10000]
     * - stream_mode 必须为 false（OrkFrame/AEAD 要求消息模式帧定界）
     * - receive_window >= send_window（跨字段约束）
     *
     * 失败抛出带字段名的 std::runtime_error。
     */
    void Validate() const
    {
        if (nodelay < 0 || nodelay > 2)
            throw std::runtime_error("KcpConfig.nodelay must be 0, 1, or 2 (got " + std::to_string(nodelay) + ")");
        if (interval_ms < 10 || interval_ms > 5000)
            throw std::runtime_error("KcpConfig.interval_ms must be in [10, 5000] (got " + std::to_string(interval_ms) + ")");
        if (fast_resend < 0 || fast_resend > 128)
            throw std::runtime_error("KcpConfig.fast_resend must be in [0, 128] (got " + std::to_string(fast_resend) + ")");
        if (send_window < 1 || send_window > 8192)
            throw std::runtime_error("KcpConfig.send_window must be in [1, 8192] (got " + std::to_string(send_window) + ")");
        if (receive_window < 1 || receive_window > 8192)
            throw std::runtime_error("KcpConfig.receive_window must be in [1, 8192] (got " + std::to_string(receive_window) + ")");
        if (rx_min_rto_ms < 1 || rx_min_rto_ms > 10000)
            throw std::runtime_error("KcpConfig.rx_min_rto_ms must be in [1, 10000] (got " + std::to_string(rx_min_rto_ms) + ")");
        if (stream_mode)
            throw std::runtime_error("KcpConfig.stream_mode must be false: OrkFrame/AEAD requires message-mode framing");
        if (receive_window < send_window)
            throw std::runtime_error("KcpConfig.receive_window must be >= send_window");
    }
};

}  // namespace ork::transport
