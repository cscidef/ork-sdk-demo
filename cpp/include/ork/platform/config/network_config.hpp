#pragma once

#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/platform/data/kcp_config.hpp"

#include <stdexcept>
#include <string>

struct NetworkConfig
{
    /**
     * 共享单包 payload 上限（字节），同时作为 ikcp_setmtu 的 MTU 参数
     * 与 Reliable/Unreliable 两通道的应用 payload 上限。
     *
     * 合法范围 [kKcpMtuMin(50), kKcpMtuMax(1414)]：
     * - 下限 50：ikcp.c ikcp_setmtu 硬约束（mtu < 50 → return -1）
     * - 上限 1414：两通道无 IPv6 分片边界中更严格者（Unreliable 加密最坏）：
     *   Unreliable wire = 48(IPv6+UDP) + 7(UdpDataHeader) + 15(OrkFrame header)
     *                     + 16(AEAD tag) + payload = 86 + payload ≤ 1500
     *   → payload ≤ 1414
     *   Reliable 单通道理论上限为 1452（48 + mtu ≤ 1500，mtu 已含 24B KCP
     *   header，不双算；见 kKcpReliableMtuMax），但共享配置不得取
     *   [1415, 1452]——该区间会令 Unreliable 加密最坏 wire 超过 1500。
     *   未加密仅少 16B AEAD tag，校验按加密最坏情况进行。
     *
     * 注：payload 靠近上限时，OrkFrame + AEAD 开销会使 KCP 分片为多个 segment，
     * 每个 segment datagram 仍不超 1500B；不承诺单 datagram 传输。
     */
    int max_packet_size{ork::transport::kDefaultMaxPacketSize};
    /** true：Bootstrap 后 KCP/UDP 走 OrkCipher AEAD；false：明文 Protobuf 透传。 */
    bool enable_encryption{false};
    /** AEAD key 有效期（秒），仅 enable_encryption=true 时生效。 */
    int bootstrap_ttl_sec{300};
    /**
     * 加解密调试日志。false=关闭（默认/生产）。
     * true=且连接已开启加密时，Seal/Open 写 Debug 日志（仅帧元数据：
     * conn/ch/seq/长度与失败原因；不输出 token、密钥材料或载荷/帧 hex）。
     * 须 log.yaml level≥Debug 才可见。
     */
    bool cipher_debug_log{false};

    // ===== Phase 4.1：KCP 协议参数 =====
    // 默认值严格等于 Phase 2 硬编码；详见 kcp_config.hpp。

    /** KCP nodelay 模式：0=关闭，1=无延迟，2=最快。默认 2。 */
    int kcp_nodelay{2};
    /** KCP 内部 flush 间隔（ms）。默认 10。 */
    int kcp_interval_ms{10};
    /** 快速重传阈值。默认 2。 */
    int kcp_fast_resend{2};
    /** true=关闭拥塞控制。默认 true。 */
    bool kcp_no_congestion{true};
    /** 发送窗口（段数）。默认 128。 */
    int kcp_send_window{128};
    /** 接收窗口（段数）。默认 128。 */
    int kcp_receive_window{128};
    /** true=流模式。默认 false（消息模式）。 */
    bool kcp_stream_mode{false};
    /** 最小 RTO（ms）。默认 10。 */
    int kcp_rx_min_rto_ms{10};

    // ===== Phase 4.4：UDP socket 内核缓冲区 =====
    // 0 = 沿用 OS 默认（不显式 setsockopt）；>0 = 请求值（OS 可能 clamp，
    // 实际生效值由 UdpTransportServer::Start() 读取并写日志）。

    /** UDP socket 接收缓冲区字节数；0=沿用 OS 默认。 */
    int udp_receive_buffer_bytes{0};
    /** UDP socket 发送缓冲区字节数；0=沿用 OS 默认。 */
    int udp_send_buffer_bytes{0};

    // ===== Phase 4.5：连接空闲超时 =====
    /**
     * 连接空闲超时（秒），超时后自动断开（DisconnectReason::IdleTimeout）。
     * 0 表示禁用空闲检测。由 TransportConfig::connectionIdleTimeoutSec 消费，
     * examples 不再硬编码 300。
     */
    int connection_idle_timeout_sec{300};

    // ===== A0.2：Bootstrap 资源保护（连接接纳门禁） =====
    // BootstrapSecurityDesign 第八章：TCP accept 总并发、未完成 Bootstrap
    // （TcpOnly）上限、每 IP 与全局新连接速率（token bucket）。
    // 判定位置：TcpTransportServer::DoAccept 的 accept 完成回调（strand 内），
    // 拒绝时静默关闭 socket，不分配 connectionId/sessionToken。

    /**
     * TCP 活跃连接（state != Disconnected）总上限。默认 2048。
     * 含未完成 Bootstrap 的连接；超出直接拒绝 accept。
     */
    int tcp_max_active_connections{2048};

    /**
     * 未完成 Bootstrap（TcpOnly / UdpHandshaking）连接上限。默认 256。
     * 必须满足 0 < tcp_max_pending_bootstrap <= tcp_max_active_connections。
     */
    int tcp_max_pending_bootstrap{256};

    /** per-IP 新连接 token bucket 容量（burst）。默认 20。 */
    int per_ip_new_conn_burst{20};

    /**
     * per-IP 持续补充速率（新连接数/分钟）。默认 60。
     * 注意：公网部署若所有客户端经同一 NAT 出口，需按实际并发调大，
     * 线下赛事局域网直连场景（每客户端独立 IP）默认值即可。
     */
    int per_ip_new_conn_rate_per_min{60};

    /**
     * 全局新连接 token bucket 容量（burst）。默认 100。
     * 依据：覆盖服务器重启后 ≤100 客户端瞬时重连（线下赛事 8 人房 ×
     * 12 房 ≈ 96，取整），不被正常重连风暴误伤。
     */
    int global_new_conn_burst{100};

    /**
     * 全局持续补充速率（新连接数/分钟）。默认 600（=10/s）。
     * 依据：per-IP 默认 60/min 的 10 倍（需 ≥10 个不同 IP 同时满速重连才
     * 触顶，不阻碍真实多用户接入）；同时 10/s 远低于 accept 循环消化能力
     * 与 TcpOnly 队列（默认 256）的回收速率，足以挡住连接洪泛把未完成
     * Bootstrap 队列瞬间打满。
     */
    int global_new_conn_rate_per_min{600};

    /**
     * TcpOnly（未完成 Bootstrap）停留超时（秒），超时后回收断开。默认 30。
     * 独立于 connection_idle_timeout_sec；0 表示禁用。
     */
    int tcp_only_timeout_sec{30};

    // ===== A1.1：Bootstrap 通道安全（TLS 1.3 / 明文开发模式） =====
    // BootstrapSecurityDesign Rev 3 第七章：生产默认 TlsServerAuthenticated；
    // PlaintextDevelopment 仅在 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 构建编入，
    // 且服务端 bind 与客户端目标强制 loopback。无 TLS 失败后的明文回退。

    /**
     * Bootstrap 通道安全模式。默认 TlsServerAuthenticated（生产安全默认）。
     * YAML：bootstrap_security: tls | plaintext_dev。
     */
    ork::platform::BootstrapSecurityMode bootstrap_security{
        ork::platform::BootstrapSecurityMode::TlsServerAuthenticated};

    /**
     * TLS 模式：PEM 证书链文件路径（含中间链，服务端发送顺序）。
     * TlsServerAuthenticated 必填，缺失 fail closed。
     */
    std::string tls_certificate_file;

    /**
     * TLS 模式：PEM 私钥文件路径。必填；加载前 POSIX 权限检查
     * （owner-only，不含 group/other 位），不合规 fail closed。
     */
    std::string tls_private_key_file;

    /**
     * TLS 握手超时（ms），超时即断开、释放槽位。默认 5000。
     * 握手期间连接纳入 A0.2 未完成 Bootstrap 预算（pendingTls）。
     */
    int tls_handshake_timeout_ms{5000};

    /**
     * @brief 构造 KcpConfig 值结构（从 NetworkConfig 扁平字段聚合）
     */
    ork::transport::KcpConfig ToKcpConfig() const
    {
        ork::transport::KcpConfig kcp;
        kcp.nodelay        = kcp_nodelay;
        kcp.interval_ms    = kcp_interval_ms;
        kcp.fast_resend    = kcp_fast_resend;
        kcp.no_congestion  = kcp_no_congestion;
        kcp.send_window    = kcp_send_window;
        kcp.receive_window = kcp_receive_window;
        kcp.stream_mode    = kcp_stream_mode;
        kcp.rx_min_rto_ms  = kcp_rx_min_rto_ms;
        return kcp;
    }

    /**
     * @brief 校验 NetworkConfig 所有字段（含 KCP 参数）
     *
     * - max_packet_size ∈ [kKcpMtuMin, kKcpMtuMax]
     * - bootstrap_ttl_sec ≥ 0
     * - A0.2 资源保护字段：tcp_max_active_connections > 0、
     *   0 < tcp_max_pending_bootstrap <= tcp_max_active_connections、
     *   per-IP / 全局 burst 与速率 > 0、tcp_only_timeout_sec >= 0
     * - A1.1 安全字段：tls_handshake_timeout_ms > 0；
     *   PlaintextDevelopment 在未编入（ORK_ALLOW_PLAINTEXT_BOOTSTRAP=OFF
     *   构建）时拒绝；TlsServerAuthenticated 要求证书链与私钥路径非空
     *   （文件存在性与私钥权限在 transport 装配时检查，同样 fail closed）
     * - 所有 KCP 字段委托给 KcpConfig::Validate()（带字段名诊断）
     */
    void Validate() const
    {
        using ork::transport::kKcpMtuMin;
        using ork::transport::kKcpMtuMax;

        if (max_packet_size < kKcpMtuMin || max_packet_size > kKcpMtuMax)
            throw std::runtime_error(
                "NetworkConfig.max_packet_size must be in [" +
                std::to_string(kKcpMtuMin) + ", " + std::to_string(kKcpMtuMax) +
                "] (got " + std::to_string(max_packet_size) + ")");

        if (bootstrap_ttl_sec < 0)
            throw std::runtime_error(
                "NetworkConfig.bootstrap_ttl_sec must be >= 0 (got " +
                std::to_string(bootstrap_ttl_sec) + ")");

        if (udp_receive_buffer_bytes < 0)
            throw std::runtime_error(
                "NetworkConfig.udp_receive_buffer_bytes must be >= 0 (got " +
                std::to_string(udp_receive_buffer_bytes) + ")");

        if (udp_send_buffer_bytes < 0)
            throw std::runtime_error(
                "NetworkConfig.udp_send_buffer_bytes must be >= 0 (got " +
                std::to_string(udp_send_buffer_bytes) + ")");

        if (connection_idle_timeout_sec < 0)
            throw std::runtime_error(
                "NetworkConfig.connection_idle_timeout_sec must be >= 0 (got " +
                std::to_string(connection_idle_timeout_sec) + ")");

        // A0.2：资源保护字段（正值 + 未完成上限不超过总上限 + 合理边界）
        if (tcp_max_active_connections <= 0)
            throw std::runtime_error(
                "NetworkConfig.tcp_max_active_connections must be > 0 (got " +
                std::to_string(tcp_max_active_connections) + ")");

        if (tcp_max_pending_bootstrap <= 0)
            throw std::runtime_error(
                "NetworkConfig.tcp_max_pending_bootstrap must be > 0 (got " +
                std::to_string(tcp_max_pending_bootstrap) + ")");

        if (tcp_max_pending_bootstrap > tcp_max_active_connections)
            throw std::runtime_error(
                "NetworkConfig.tcp_max_pending_bootstrap (" +
                std::to_string(tcp_max_pending_bootstrap) +
                ") must be <= tcp_max_active_connections (" +
                std::to_string(tcp_max_active_connections) + ")");

        if (per_ip_new_conn_burst <= 0)
            throw std::runtime_error(
                "NetworkConfig.per_ip_new_conn_burst must be > 0 (got " +
                std::to_string(per_ip_new_conn_burst) + ")");

        if (per_ip_new_conn_rate_per_min <= 0)
            throw std::runtime_error(
                "NetworkConfig.per_ip_new_conn_rate_per_min must be > 0 (got " +
                std::to_string(per_ip_new_conn_rate_per_min) + ")");

        if (global_new_conn_burst <= 0)
            throw std::runtime_error(
                "NetworkConfig.global_new_conn_burst must be > 0 (got " +
                std::to_string(global_new_conn_burst) + ")");

        if (global_new_conn_rate_per_min <= 0)
            throw std::runtime_error(
                "NetworkConfig.global_new_conn_rate_per_min must be > 0 (got " +
                std::to_string(global_new_conn_rate_per_min) + ")");

        if (tcp_only_timeout_sec < 0)
            throw std::runtime_error(
                "NetworkConfig.tcp_only_timeout_sec must be >= 0 (got " +
                std::to_string(tcp_only_timeout_sec) + ")");

        // A1.1：Bootstrap 通道安全字段
        if (tls_handshake_timeout_ms <= 0)
            throw std::runtime_error(
                "NetworkConfig.tls_handshake_timeout_ms must be > 0 (got " +
                std::to_string(tls_handshake_timeout_ms) + ")");

        if (bootstrap_security == ork::platform::BootstrapSecurityMode::PlaintextDevelopment)
        {
            if (!ork::platform::kPlaintextBootstrapCompiledIn)
                throw std::runtime_error(
                    "NetworkConfig.bootstrap_security=plaintext_dev is not compiled into "
                    "this build (production default ORK_ALLOW_PLAINTEXT_BOOTSTRAP=OFF). "
                    "Use bootstrap_security: tls with certificate/private key, or rebuild "
                    "tests/dev binaries with -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON (loopback "
                    "only). No plaintext fallback exists.");
        }
        else if (bootstrap_security ==
                 ork::platform::BootstrapSecurityMode::TlsServerAuthenticated)
        {
            if (tls_certificate_file.empty())
                throw std::runtime_error(
                    "NetworkConfig.tls_certificate_file is required when "
                    "bootstrap_security=tls (fail closed; no plaintext fallback)");
            if (tls_private_key_file.empty())
                throw std::runtime_error(
                    "NetworkConfig.tls_private_key_file is required when "
                    "bootstrap_security=tls (fail closed; no plaintext fallback)");
        }
        else
        {
            throw std::runtime_error(
                "NetworkConfig.bootstrap_security has unknown value " +
                std::string(ork::platform::ToString(bootstrap_security)));
        }

        // 委托 KcpConfig::Validate — 失败时带字段名
        ToKcpConfig().Validate();
    }
};
