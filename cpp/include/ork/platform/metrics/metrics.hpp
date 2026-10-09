#pragma once
/**
 * @file metrics.hpp
 * @brief 网络统计单写者计数器（Phase 4.5：strand 内单写，快照读）
 *
 * 线程模型（Phase 4.2-4.5）：
 * - 所有 transport 状态变更收归到 NetworkPump 的单一 asio strand；
 * - Metrics 的**写**调用点（Inc / Add / Set 系列）全部发生在该 strand 内，为单写者；
 * - 计数字段仍为 std::atomic，允许外部线程（如 gRPC HealthCheck、宿主查询）
 *   并发**读** Snapshot()，无需额外同步；
 * - Snapshot() 逐字段 load，非事务一致（各计数器采样瞬间可能来自不同时刻），
 *   这对监控用途足够，不承诺跨字段原子性。
 *
 * 语义约定：
 * - packets_in / packets_out、bytes_in / bytes_out 为 **wire 级**累计（收发网卡字节/包），
 *   而非解码后的应用消息数；
 * - kcp_* 为跨当前存活 KCP 会话的聚合瞬时值（每次宿主 pump 重算）；
 * - *_queue_* 为队列水位（瞬时长度 + 历史高水位）。
 */
#include <atomic>
#include <array>
#include <cstdint>

#include "ork/platform/data/disconnect_reason.hpp"

struct MetricsData
{
    // ===== pump / tick =====
    std::atomic<uint64_t> tick_count{0};       ///< 宿主 pump 迭代数（pumpTickCount）
    std::atomic<uint64_t> tick_lag_count{0};   ///< pump 帧超限次数（pumpOverrunCount）
    std::atomic<uint64_t> worker_queue_length{0};

    // ===== wire 级收发累计 =====
    std::atomic<uint64_t> recv_packets{0};     ///< 入站 wire 包数（packetsIn）
    std::atomic<uint64_t> send_packets{0};     ///< 出站 wire 包数（packetsOut）
    std::atomic<uint64_t> bytes_in{0};         ///< 入站 wire 字节（bytesIn）
    std::atomic<uint64_t> bytes_out{0};        ///< 出站 wire 字节（bytesOut）

    // ===== 丢弃 =====
    std::atomic<uint64_t> drops_too_large{0};  ///< Sender 超限丢弃
    std::atomic<uint64_t> drops_queue_full{0}; ///< UDP 发送队列满丢弃

    // ===== 队列水位 =====
    std::atomic<uint64_t> udp_send_queue_len{0};       ///< UDP 发送队列瞬时长度
    std::atomic<uint64_t> udp_send_queue_high_water{0};//< UDP 发送队列历史高水位
    std::atomic<uint64_t> command_queue_len{0};        ///< 入站批次瞬时长度（待分发）
    std::atomic<uint64_t> inbound_queue_high_water{0}; ///< 入站批次历史高水位

    // ===== KCP 聚合（跨存活会话，每次 pump 重算） =====
    std::atomic<uint64_t> kcp_waitsnd_sum{0};       ///< 各会话 waitsnd 之和
    std::atomic<uint64_t> kcp_srtt_avg_ms{0};       ///< 各会话 srtt 平均（ms）
    std::atomic<uint64_t> kcp_retransmits_total{0}; ///< 各会话 RTO 重传之和

    // ===== 断开统计（按 DisconnectReason 索引） =====
    std::array<std::atomic<uint64_t>, kDisconnectReasonCount> disconnects_by_reason{};

    // ===== 加密失败（OrkCipher::Open 分类） =====
    std::atomic<uint64_t> crypto_fail_aead{0};
    std::atomic<uint64_t> crypto_fail_replay{0};
    std::atomic<uint64_t> crypto_fail_expired{0};
    std::atomic<uint64_t> crypto_fail_malformed{0};

    // ===== 解码失败（非加密的帧/缓冲解码错误） =====
    std::atomic<uint64_t> decode_failures{0};

    // ===== A0.2：Bootstrap 资源保护 =====
    // 拒绝路径静默（不逐连接写日志，防日志盘 DoS），只聚合计数；
    // gauge 由 HostPump 每拍从 ConnectionManager 原子计数采集。
    std::atomic<uint64_t> tcp_active_connections{0};        ///< gauge：活跃 TCP 连接（state != Disconnected）
    std::atomic<uint64_t> tcp_pending_bootstrap{0};         ///< gauge：未完成 Bootstrap（TcpOnly）连接
    std::atomic<uint64_t> conn_rejected_total{0};           ///< 所有拒绝类别之和
    std::atomic<uint64_t> conn_rejected_per_ip{0};          ///< per-IP token bucket 拒绝（含 map 满）
    std::atomic<uint64_t> conn_rejected_global_rate{0};     ///< 全局 token bucket 拒绝
    std::atomic<uint64_t> conn_rejected_max_concurrent{0};  ///< TCP 活跃总上限拒绝
    std::atomic<uint64_t> conn_rejected_pending_bootstrap{0}; ///< 未完成 Bootstrap 上限拒绝
    std::atomic<uint64_t> tcp_only_timeout_recycles{0};     ///< TcpOnly 停留超时回收数

    // ===== A1.1：Bootstrap TLS 1.3 =====
    // 握手失败路径静默（不逐连接写日志，同 A0.2 拒绝路径），仅聚合计数；
    // tls_pending gauge 由 HostPump 每拍从 TcpTransportServer 采集。
    std::atomic<uint64_t> tls_handshakes_succeeded{0};  ///< TLS 握手成功（含重连）
    std::atomic<uint64_t> tls_handshakes_failed{0};     ///< TLS 握手失败（非超时：
                                                        ///< 证书校验/协议/对端中断）
    std::atomic<uint64_t> tls_handshake_timeouts{0};    ///< TLS 握手超时（独立 timer）
    std::atomic<uint64_t> tls_pending{0};               ///< gauge：正在握手、尚未上报的连接

    // ===== B1：UDP Handshake V2 首次绑定（Rev 3 §5.7） =====
    // 拒绝路径静默（不逐包写日志，防日志盘 DoS），只聚合计数 + 调用方 1/64
    // 采样日志；未知 connectionId 不做 HMAC（资源保护）。
    std::atomic<uint64_t> udp_hs_v2_succeeded{0};          ///< 首次合法绑定（Connected 仅一次）
    std::atomic<uint64_t> udp_hs_v2_acks_resent{0};        ///< 同 nonce 更大 counter：仅重发 ACK
    std::atomic<uint64_t> udp_hs_v2_v1_rejected{0};        ///< 旧 9B Handshake V1 静默拒绝（硬切）
    std::atomic<uint64_t> udp_hs_v2_unknown_connection{0}; ///< connectionId 未命中（未做 HMAC）
    std::atomic<uint64_t> udp_hs_v2_invalid_tag{0};        ///< authTag 验证失败 / 基础解码拒绝
    std::atomic<uint64_t> udp_hs_v2_key_expired{0};        ///< Bootstrap TTL 过期后收到握手
    std::atomic<uint64_t> udp_hs_v2_replay_rejected{0};    ///< 同 nonce 相同/更小 counter（重放）
    std::atomic<uint64_t> udp_hs_v2_nonce_mismatch{0};     ///< 不同 nonce（B1 静默拒绝，B2 才挑战）
    std::atomic<uint64_t> udp_hs_v2_endpoint_mismatch{0};  ///< 已绑定 endpoint 来源不同（拒绝换绑）

    MetricsData() = default;

    MetricsData(const MetricsData& other)
        : tick_count(other.tick_count.load()),
          tick_lag_count(other.tick_lag_count.load()),
          worker_queue_length(other.worker_queue_length.load()),
          recv_packets(other.recv_packets.load()),
          send_packets(other.send_packets.load()),
          bytes_in(other.bytes_in.load()),
          bytes_out(other.bytes_out.load()),
          drops_too_large(other.drops_too_large.load()),
          drops_queue_full(other.drops_queue_full.load()),
          udp_send_queue_len(other.udp_send_queue_len.load()),
          udp_send_queue_high_water(other.udp_send_queue_high_water.load()),
          command_queue_len(other.command_queue_len.load()),
          inbound_queue_high_water(other.inbound_queue_high_water.load()),
          kcp_waitsnd_sum(other.kcp_waitsnd_sum.load()),
          kcp_srtt_avg_ms(other.kcp_srtt_avg_ms.load()),
          kcp_retransmits_total(other.kcp_retransmits_total.load()),
          crypto_fail_aead(other.crypto_fail_aead.load()),
          crypto_fail_replay(other.crypto_fail_replay.load()),
          crypto_fail_expired(other.crypto_fail_expired.load()),
          crypto_fail_malformed(other.crypto_fail_malformed.load()),
          decode_failures(other.decode_failures.load()),
          tcp_active_connections(other.tcp_active_connections.load()),
          tcp_pending_bootstrap(other.tcp_pending_bootstrap.load()),
          conn_rejected_total(other.conn_rejected_total.load()),
          conn_rejected_per_ip(other.conn_rejected_per_ip.load()),
          conn_rejected_global_rate(other.conn_rejected_global_rate.load()),
          conn_rejected_max_concurrent(other.conn_rejected_max_concurrent.load()),
          conn_rejected_pending_bootstrap(other.conn_rejected_pending_bootstrap.load()),
          tcp_only_timeout_recycles(other.tcp_only_timeout_recycles.load()),
          tls_handshakes_succeeded(other.tls_handshakes_succeeded.load()),
          tls_handshakes_failed(other.tls_handshakes_failed.load()),
          tls_handshake_timeouts(other.tls_handshake_timeouts.load()),
          tls_pending(other.tls_pending.load()),
          udp_hs_v2_succeeded(other.udp_hs_v2_succeeded.load()),
          udp_hs_v2_acks_resent(other.udp_hs_v2_acks_resent.load()),
          udp_hs_v2_v1_rejected(other.udp_hs_v2_v1_rejected.load()),
          udp_hs_v2_unknown_connection(other.udp_hs_v2_unknown_connection.load()),
          udp_hs_v2_invalid_tag(other.udp_hs_v2_invalid_tag.load()),
          udp_hs_v2_key_expired(other.udp_hs_v2_key_expired.load()),
          udp_hs_v2_replay_rejected(other.udp_hs_v2_replay_rejected.load()),
          udp_hs_v2_nonce_mismatch(other.udp_hs_v2_nonce_mismatch.load()),
          udp_hs_v2_endpoint_mismatch(other.udp_hs_v2_endpoint_mismatch.load())
    {
        for (std::size_t i = 0; i < kDisconnectReasonCount; ++i)
            disconnects_by_reason[i].store(other.disconnects_by_reason[i].load());
    }
};

class Metrics
{
public:
    static Metrics& Instance()
    {
        static Metrics instance;
        return instance;
    }

    // ---- Tick / pump ----
    void IncTickCount(uint64_t n = 1)
    {
        data_.tick_count += n;
    }
    void IncTickLag(uint64_t n = 1)
    {
        data_.tick_lag_count += n;
    }

    // ---- wire 级收发 ----
    void AddRecvPackets(uint64_t n)
    {
        data_.recv_packets += n;
    }
    void AddSendPackets(uint64_t n)
    {
        data_.send_packets += n;
    }
    void AddBytesIn(uint64_t n)
    {
        data_.bytes_in += n;
    }
    void AddBytesOut(uint64_t n)
    {
        data_.bytes_out += n;
    }

    // ---- 丢弃 ----
    void IncDropsTooLarge(uint64_t n = 1)
    {
        data_.drops_too_large += n;
    }
    void IncDropsQueueFull(uint64_t n = 1)
    {
        data_.drops_queue_full += n;
    }

    // ---- 队列水位 ----
    void SetWorkerQueueLength(uint64_t length)
    {
        data_.worker_queue_length = length;
    }
    void SetUdpSendQueueLen(uint64_t len)
    {
        data_.udp_send_queue_len = len;
    }
    /// 更新 UDP 发送队列高水位（单调递增，取历史峰值）
    void UpdateUdpSendQueueHighWater(uint64_t len)
    {
        uint64_t cur = data_.udp_send_queue_high_water.load(std::memory_order_relaxed);
        while (len > cur &&
               !data_.udp_send_queue_high_water.compare_exchange_weak(
                   cur, len, std::memory_order_relaxed))
        {
        }
    }
    void SetCommandQueueLen(uint64_t len)
    {
        data_.command_queue_len = len;
    }
    void UpdateInboundQueueHighWater(uint64_t len)
    {
        uint64_t cur = data_.inbound_queue_high_water.load(std::memory_order_relaxed);
        while (len > cur &&
               !data_.inbound_queue_high_water.compare_exchange_weak(
                   cur, len, std::memory_order_relaxed))
        {
        }
    }

    // ---- KCP 聚合 ----
    void SetKcpAggregate(uint64_t waitsndSum, uint64_t srttAvgMs, uint64_t retransmitsTotal)
    {
        data_.kcp_waitsnd_sum = waitsndSum;
        data_.kcp_srtt_avg_ms = srttAvgMs;
        data_.kcp_retransmits_total = retransmitsTotal;
    }

    // ---- 断开统计 ----
    void IncDisconnect(DisconnectReason reason)
    {
        data_.disconnects_by_reason[DisconnectReasonIndex(reason)] += 1;
    }

    // ---- 加密失败 ----
    void IncCryptoFailAead(uint64_t n = 1) { data_.crypto_fail_aead += n; }
    void IncCryptoFailReplay(uint64_t n = 1) { data_.crypto_fail_replay += n; }
    void IncCryptoFailExpired(uint64_t n = 1) { data_.crypto_fail_expired += n; }
    void IncCryptoFailMalformed(uint64_t n = 1) { data_.crypto_fail_malformed += n; }

    // ---- 解码失败 ----
    void IncDecodeFailures(uint64_t n = 1) { data_.decode_failures += n; }

    // ---- A0.2：资源保护（拒绝静默，仅聚合计数；gauge 由 HostPump 采集） ----
    void SetTcpActiveConnections(uint64_t count) { data_.tcp_active_connections = count; }
    void SetTcpPendingBootstrap(uint64_t count) { data_.tcp_pending_bootstrap = count; }
    void IncConnRejectedTotal(uint64_t n = 1) { data_.conn_rejected_total += n; }
    void IncConnRejectedPerIp(uint64_t n = 1) { data_.conn_rejected_per_ip += n; }
    void IncConnRejectedGlobalRate(uint64_t n = 1) { data_.conn_rejected_global_rate += n; }
    void IncConnRejectedMaxConcurrent(uint64_t n = 1) { data_.conn_rejected_max_concurrent += n; }
    void IncConnRejectedPendingBootstrap(uint64_t n = 1) { data_.conn_rejected_pending_bootstrap += n; }
    void IncTcpOnlyTimeoutRecycles(uint64_t n = 1) { data_.tcp_only_timeout_recycles += n; }

    // ---- A1.1：Bootstrap TLS（失败静默，仅聚合计数；gauge 由 HostPump 采集） ----
    void IncTlsHandshakesSucceeded(uint64_t n = 1) { data_.tls_handshakes_succeeded += n; }
    void IncTlsHandshakesFailed(uint64_t n = 1) { data_.tls_handshakes_failed += n; }
    void IncTlsHandshakeTimeouts(uint64_t n = 1) { data_.tls_handshake_timeouts += n; }
    void SetTlsPending(uint64_t count) { data_.tls_pending = count; }

    // ---- B1：UDP Handshake V2（拒绝静默，仅聚合计数；采样日志由调用方负责） ----
    void IncUdpHsV2Succeeded(uint64_t n = 1) { data_.udp_hs_v2_succeeded += n; }
    void IncUdpHsV2AcksResent(uint64_t n = 1) { data_.udp_hs_v2_acks_resent += n; }
    void IncUdpHsV2V1Rejected(uint64_t n = 1) { data_.udp_hs_v2_v1_rejected += n; }
    void IncUdpHsV2UnknownConnection(uint64_t n = 1) { data_.udp_hs_v2_unknown_connection += n; }
    void IncUdpHsV2InvalidTag(uint64_t n = 1) { data_.udp_hs_v2_invalid_tag += n; }
    void IncUdpHsV2KeyExpired(uint64_t n = 1) { data_.udp_hs_v2_key_expired += n; }
    void IncUdpHsV2ReplayRejected(uint64_t n = 1) { data_.udp_hs_v2_replay_rejected += n; }
    void IncUdpHsV2NonceMismatch(uint64_t n = 1) { data_.udp_hs_v2_nonce_mismatch += n; }
    void IncUdpHsV2EndpointMismatch(uint64_t n = 1) { data_.udp_hs_v2_endpoint_mismatch += n; }

    // ---- Snapshot（外部线程可读） ----
    MetricsData Snapshot() const
    {
        MetricsData snap;
        snap.tick_count = data_.tick_count.load();
        snap.tick_lag_count = data_.tick_lag_count.load();
        snap.worker_queue_length = data_.worker_queue_length.load();
        snap.recv_packets = data_.recv_packets.load();
        snap.send_packets = data_.send_packets.load();
        snap.bytes_in = data_.bytes_in.load();
        snap.bytes_out = data_.bytes_out.load();
        snap.drops_too_large = data_.drops_too_large.load();
        snap.drops_queue_full = data_.drops_queue_full.load();
        snap.udp_send_queue_len = data_.udp_send_queue_len.load();
        snap.udp_send_queue_high_water = data_.udp_send_queue_high_water.load();
        snap.command_queue_len = data_.command_queue_len.load();
        snap.inbound_queue_high_water = data_.inbound_queue_high_water.load();
        snap.kcp_waitsnd_sum = data_.kcp_waitsnd_sum.load();
        snap.kcp_srtt_avg_ms = data_.kcp_srtt_avg_ms.load();
        snap.kcp_retransmits_total = data_.kcp_retransmits_total.load();
        for (std::size_t i = 0; i < kDisconnectReasonCount; ++i)
            snap.disconnects_by_reason[i] = data_.disconnects_by_reason[i].load();
        snap.crypto_fail_aead = data_.crypto_fail_aead.load();
        snap.crypto_fail_replay = data_.crypto_fail_replay.load();
        snap.crypto_fail_expired = data_.crypto_fail_expired.load();
        snap.crypto_fail_malformed = data_.crypto_fail_malformed.load();
        snap.decode_failures = data_.decode_failures.load();
        snap.tcp_active_connections = data_.tcp_active_connections.load();
        snap.tcp_pending_bootstrap = data_.tcp_pending_bootstrap.load();
        snap.conn_rejected_total = data_.conn_rejected_total.load();
        snap.conn_rejected_per_ip = data_.conn_rejected_per_ip.load();
        snap.conn_rejected_global_rate = data_.conn_rejected_global_rate.load();
        snap.conn_rejected_max_concurrent = data_.conn_rejected_max_concurrent.load();
        snap.conn_rejected_pending_bootstrap = data_.conn_rejected_pending_bootstrap.load();
        snap.tcp_only_timeout_recycles = data_.tcp_only_timeout_recycles.load();
        snap.tls_handshakes_succeeded = data_.tls_handshakes_succeeded.load();
        snap.tls_handshakes_failed = data_.tls_handshakes_failed.load();
        snap.tls_handshake_timeouts = data_.tls_handshake_timeouts.load();
        snap.tls_pending = data_.tls_pending.load();
        snap.udp_hs_v2_succeeded = data_.udp_hs_v2_succeeded.load();
        snap.udp_hs_v2_acks_resent = data_.udp_hs_v2_acks_resent.load();
        snap.udp_hs_v2_v1_rejected = data_.udp_hs_v2_v1_rejected.load();
        snap.udp_hs_v2_unknown_connection = data_.udp_hs_v2_unknown_connection.load();
        snap.udp_hs_v2_invalid_tag = data_.udp_hs_v2_invalid_tag.load();
        snap.udp_hs_v2_key_expired = data_.udp_hs_v2_key_expired.load();
        snap.udp_hs_v2_replay_rejected = data_.udp_hs_v2_replay_rejected.load();
        snap.udp_hs_v2_nonce_mismatch = data_.udp_hs_v2_nonce_mismatch.load();
        snap.udp_hs_v2_endpoint_mismatch = data_.udp_hs_v2_endpoint_mismatch.load();
        return snap;
    }

private:
    Metrics() = default;
    MetricsData data_;
};
