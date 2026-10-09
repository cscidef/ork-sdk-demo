#pragma once
#include "ork/connection/connection_manager.hpp"
#include "ork/platform/context/network_context.hpp"
#include "ork/platform/context/runtime_context.hpp"
#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/transport/data/inout_bound_message.hpp"
#include "ork/transport/data/network_defs.hpp"
#include "ork/transport/network_pump.hpp"
#include "ork/platform/data/disconnect_reason.hpp"
#include "ork/platform/data/transport_channel.hpp"
#include "ork/platform/data/kcp_config.hpp"
#include "ork/transport/transport_tcp/tcp_transport_server.hpp"
#include "ork/transport/transport_tcp/connection_rate_limiter.hpp"
#include "ork/transport/transport_udp/udp_transport_server.hpp"
#include "ork/platform/data/send_result.hpp"
#include <array>
#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declaration — kcp_session.hpp 依赖 ikcp.h，不入 SDK 包
class KcpSession;

/**
 * @brief 网络传输门面（Phase 4.3: 双频调度，全 strand 内）
 *
 * 所有 transport 状态变更收归到 NetworkPump 的单一 strand：
 * - TCP/UDP 回调绑定 strand
 * - KCP Input/Update/Send/Recv 在 strand 内执行
 * - ConnectionManager 写操作在 strand 内
 * - UDP 发送走 strand 私有队列
 *
 * 外部线程通过 Dispatch（fire-and-forget）或 Invoke（同步桥接）访问。
 *
 * Phase 4.3 双频调度：
 * - strand timer 以 kcpUpdateRate（默认 100Hz=10ms）驱动 KCP Update / Recv；
 * - 每拍累加 kcpIntervalMs，累加值 >= hostIntervalMs（1000/pumpRate）时触发
 *   一次 HostPump：空闲检测 + 快照发布 + 宿主回调（IHostListener 分发）+ 统计；
 * - 入站消息不再跨线程：IO/KCP 回调直接追加到 strand 私有 pendingInbound_，
 *   HostPump 时交换出缓冲区交给宿主回调（ConcurrentQueue 边界已消除）。
 */
class NetworkTransport
{
public:
    /// @param kcpUpdateIntervalMs  strand timer 拍间隔（KCP 驱动频率，1000/kcpUpdateRate）
    /// @param hostPumpIntervalMs   宿主 pump 间隔（1000/pumpRate），经累加器分频触发
    explicit NetworkTransport(asio::io_context& io, Port tcpPort, Port udpPort,
                              ConnectionManager& connectionMgr,
                              uint32_t kcpUpdateIntervalMs = 10,
                              uint32_t hostPumpIntervalMs = 10);

    ~NetworkTransport();

    void Start();
    void Run();
    void Stop();

    void SetKcpEnabled(bool enabled) { kcpEnabled_ = enabled; }
    bool IsKcpEnabled() const { return kcpEnabled_; }

    /// Phase 4.2：设置空闲超时（微秒）；<=0 表示禁用空闲检测。
    /// 空闲检测由 NetworkPump timer 在 strand 内自驱动，外部只需设置阈值。
    void SetIdleTimeout(Duration idleTimeoutUs) { idleTimeoutUs_ = idleTimeoutUs; }

    /// Phase 4.1：设置 KCP 协议参数与 MTU（创建 KcpSession 时使用）
    void SetKcpConfig(const ork::transport::KcpConfig& config, int mtu);

    /// Phase 4.4：设置 UDP socket 内核缓冲区请求值（字节，<=0=OS 默认）；Start() 前调用
    void SetUdpSocketBuffers(int recvBytes, int sendBytes);

    /**
     * @brief A0.2：设置 Bootstrap 资源保护（连接接纳门禁）；Start() 前调用
     *
     * 四层判定（顺序固定，见 AdmitConnection）：TCP 活跃总上限 → 未完成
     * Bootstrap（TcpOnly）上限 → per-IP token bucket → 全局 token bucket。
     * 拒绝路径静默（仅 Metrics 聚合计数）；tcpOnlyTimeoutSec 由 HostPump
     * 定期回收存量 TcpOnly 连接。
     */
    void SetConnectionProtection(const ork::transport::ConnectionProtectionConfig& config);

    /**
     * @brief A1.1：设置 Bootstrap 通道安全（Start() 前调用；fail closed）
     *
     * - TlsServerAuthenticated：经 transport_tls 薄工厂创建 ssl::context
     *   （TLS 1.3 only、0-RTT 禁用、私钥 POSIX owner-only 权限检查），交给
     *   TcpTransportServer；accept 后先握手、成功才上报 onConnected，
     *   Bootstrap V2 在 TLS 通道内发送。证书/私钥缺失或权限不合规直接 throw。
     * - PlaintextDevelopment：仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 构建编入；
     *   服务端强制 loopback bind。未编入时 throw（fail closed，无明文回退）。
     *
     * 握手期间连接计入 pendingTls（AdmitConnection 第二层预算 + 独立超时 timer）。
     */
    void SetBootstrapSecurity(ork::platform::BootstrapSecurityMode mode,
                              const std::string& certificateFile,
                              const std::string& privateKeyFile,
                              int tlsHandshakeTimeoutMs);

    /**
     * @brief 设置宿主 pump 回调（Phase 4.3）
     *
     * 由 SessionFrameworkHost 在 Start 前注入；每次 HostPump 以 pumpRate 频率
     * 在 strand 内调用，参数为本轮积累的入站消息批次（回调返回后即丢弃）。
     * 回调内可安全重入 Disconnect/Send（新入站消息进入下一批，不会破坏本次迭代）。
     */
    void SetHostPumpCallback(std::function<void(std::vector<InboundMessage>&)> fn)
    {
        hostPumpFn_ = std::move(fn);
    }

    // ===== 发送（Phase 4.2: strand 桥接） =====

    /// 发送 opaque 字节（同步返回 SendResult）
    ork::platform::SendResult Send(ConnectionId conn, uint32_t packetType,
                                   std::span<const uint8_t> payload, TransportChannel channel);

    /// TrySend（同步返回 SendResult）
    ork::platform::SendResult TrySend(ConnectionId conn, uint32_t packetType,
                                      std::span<const uint8_t> payload, TransportChannel channel);

    // ===== 连接治理（供 SessionFramework / IConnectionRegistry 使用） =====

    /** @brief 活跃连接数（读快照，外部线程安全） */
    size_t CountActiveConnections() const;

    /** @brief 查询单条连接快照条目（外部线程安全） */
    bool FindConnectionSnapshot(ConnectionId conn, ConnectionSnapshotEntry& out) const;

    /** @brief 遍历快照中所有连接（外部线程安全） */
    void ForEachSnapshotConnection(
        const std::function<void(ConnectionId, const ConnectionSnapshotEntry&)>& fn) const;

    /**
     * @brief 主动断开指定连接（外部线程安全，post 到 strand）
     */
    void Disconnect(ConnectionId conn, DisconnectReason reason = DisconnectReason::Kicked);

    /** @brief 获取 NetworkPump 引用（供 SessionFrameworkHost 查询状态） */
    NetworkPump& GetPump() { return pump_; }

private:
    // ===== strand 内部实现 =====

    /// strand timer 每拍（kcpUpdateRate）：KCP 驱动 + 累加器分频触发 HostPump
    void OnPumpTick();

    /// 宿主 pump（pumpRate）：空闲检测 + 快照发布 + 宿主回调分发 + 统计
    void HostPump();

    void BindTcpCallbacks();
    void BindUdpCallbacks();

    /// A0.2：接纳门禁判定（strand 内，由 accept 完成回调调用）。
    /// A1.1：pendingTls（正在 TLS 握手、尚未上报的连接）一并纳入第二层
    ///（未完成 Bootstrap）预算，避免握手绕过 A0.2 门禁。
    bool AdmitConnection(const asio::ip::tcp::endpoint& remote, std::size_t pendingTls);

    void OnTcpConnected(std::shared_ptr<TcpConnection> tcp);
    void OnTcpDisconnected(ConnectionId id);
    void OnTcpRawMessage(ConnectionId id, std::vector<Byte>&& data);

    /// B1（Rev 3 §5.7）：UDP Handshake V2 处理（strand 内）。
    /// 完成基础校验 → 查表 → TTL → authTag 验证 → 首次绑定/幂等重试状态机；
    /// 失败路径静默（不回包，防 UDP 反射），仅 Metrics 计数 + 1/64 采样日志。
    void OnUdpHandshake(const asio::ip::udp::endpoint& endpoint, std::span<const uint8_t> packet);
    void OnUdpPacket(const asio::ip::udp::endpoint& endpoint, ConnectionId connectionId, std::span<const uint8_t> payload);
    void OnUdpRawPacket(const asio::ip::udp::endpoint& endpoint, std::span<const uint8_t> packet);

    /// B1：握手拒绝事件 1/64 采样日志（strand 内；不含 nonce/tag/key 等秘密值）
    void SampleLogUdpHsV2Reject(const char* reason, ConnectionId connectionId,
                                const asio::ip::udp::endpoint& endpoint);

    /// B1：发送加密 KCP ACK（"ORK-UDP-ACK:handshake"，复用既有 Reliable 通道）
    void SendHandshakeAck(ConnectionId connectionId);

    void CreateKcpSession(ConnectionId connectionId, const UdpEndpoint& remote_endpoint);
    void DestroyKcpSession(ConnectionId connectionId);
    KcpSession* FindKcpSession(ConnectionId connectionId);

    /// strand 内发送实现
    ork::platform::SendResult TrySendImpl(ConnectionId conn, uint32_t packetType,
                                          std::span<const uint8_t> payload, TransportChannel channel);
    bool SendViaTcp(ConnectionId conn, std::span<const uint8_t> payload);
    bool SendViaKcp(ConnectionId conn, uint32_t packetType, std::span<const uint8_t> payload);
    bool SendViaUdp(ConnectionId conn, uint32_t packetType, std::span<const uint8_t> payload);

    /// strand 内断开实现
    void DisconnectImpl(ConnectionId conn, DisconnectReason reason);

    /// strand 内 KCP 驱动
    void ProcessKcpUpdate();
    void ProcessKcpInbound();

    /// strand 内空闲检测
    void ProcessIdleConnections();

    /// A0.2：strand 内 TcpOnly 停留超时回收（未完成 Bootstrap 的存量清理）
    void ProcessTcpOnlyTimeouts();

    /// Phase 4.5：strand 内采集 KCP 聚合诊断（waitsnd 之和 / srtt 均值 / 重传之和）写入 Metrics
    void CollectKcpDiagnostics();

    /// Phase 4.5：strand 内采集队列水位（UDP 发送队列 + 入站批次）写入 Metrics
    void CollectQueueWatermarks();

    /// 取出主动断开时暂存的 reason（strand 内）
    DisconnectReason TakePendingReason(ConnectionId conn);

    /// Stop 时在 strand 内执行的清理序列
    void ShutdownImpl();

private:
    asio::io_context& io_;
    NetworkPump pump_;
    TcpTransportServer tcpServer_;
    UdpTransportServer udpServer_;
    ConnectionManager& connectionMgr_;

    /// Phase 4.3：宿主 pump 回调（分发入站消息给 IHostListener，strand 内）
    std::function<void(std::vector<InboundMessage>&)> hostPumpFn_;

    /// strand 私有（无锁）
    std::unordered_map<ConnectionId, std::unique_ptr<KcpSession>> kcpSessions_;
    std::unordered_map<ConnectionId, DisconnectReason> pendingReasons_;
    std::array<uint8_t, 4096> kcpRecvBuffer_{};

    /// Phase 4.3：strand 私有入站缓冲区（IO/KCP 回调追加，HostPump 交换出分发）
    std::vector<InboundMessage> pendingInbound_;

    /// Phase 4.3：双频调度累加器
    uint32_t kcpIntervalMs_ = 10;    ///< strand timer 拍间隔（KCP 驱动）
    uint32_t hostIntervalMs_ = 10;   ///< 宿主 pump 间隔（累加器阈值）
    uint32_t accumulatorMs_ = 0;     ///< 每拍累加 kcpIntervalMs_，达阈值触发 HostPump

    bool kcpEnabled_ = true;
    ork::transport::KcpConfig kcpConfig_;
    int kcpMtu_ = ork::transport::kDefaultMaxPacketSize;
    std::atomic<bool> running_{false};
    std::thread thread_;

    /// 空闲超时配置（由 SessionFrameworkImpl 设置）
    Duration idleTimeoutUs_ = 0;

    /// A0.2：资源保护配置 + 限流器（仅 strand 内访问；默认值见
    /// ConnectionProtectionConfig / network_config.hpp）
    ork::transport::ConnectionProtectionConfig protection_{};
    ork::transport::ConnectionRateLimiter rateLimiter_{};
    Duration tcpOnlyTimeoutUs_ = 30'000'000ll;  // 30s 默认（protection_.tcpOnlyTimeoutSec）

    /// B1：握手拒绝采样日志计数器（strand 私有；1/64 采样，禁止每包日志）
    uint64_t udpHsV2RejectSampleCounter_ = 0;
};
