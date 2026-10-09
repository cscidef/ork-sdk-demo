#pragma once
/**
 * @file ork_client.hpp
 * @brief OrkClient — C++ 客户端（Phase 5C.1）
 *
 * Unity AR 真机形态下客户端侧的传输实现。复用 core 现有构件：
 *   - ork::codec::BootstrapFrame / FrameType（帧编解码）
 *   - ork::codec::UdpHandshakeV2Frame（42B 认证握手帧，B1）
 *   - ork::crypto::OrkCipher（AEAD Seal/Open，方向 ClientToServer）
 *   - KcpSession（可靠通道，conv = connectionId）
 *   - UdpDataHeader（UDP 定长包头）
 *
 * 连接流程：
 *   TCP connect →（可选 TLS 1.3）→ 接收 Bootstrap V2 帧（109B，硬切单轨）→
 *   生成 clientNonce → UDP Handshake V2（42B 认证帧）→ KCP 建立 →
 *   收到服务端 "ORK-UDP-ACK:"（解密成功）→ Connected。
 *
 * B1（Rev 3 §5.6/§5.8）：
 *   - 每次成功 Bootstrap 后生成 16B clientNonce（CSPRNG）；
 *   - ACK 丢失重试复用同一 nonce、counter 严格递增（0->1->2...，不回绕）；
 *   - counter 耗尽（UINT32_MAX）fail closed 断开，须重连重走 Bootstrap；
 *   - 仅 ACK 解密成功（KCP Open 通过）才转 Connected。
 *
 * Bootstrap 硬切（A0.3，Rev 3 §5.9）：
 *   - 外层 4B 大端 length prefix 必须严格等于 109，否则立即断开；
 *   - 帧解码严格按 §5.3 顺序（magic→version→type→reserved→payloadLength）；
 *   - 不接受 V1 81B、不猜版本、不做宽容解析；失败以 ProtocolError 上报。
 *
 * ============================ 线程契约 ============================
 * OrkClient 为单线程模型，天然无需 strand：
 *   - 手动驱动：外部反复调用 PumpOnce()（如游戏主循环），所有异步回调、
 *     Send、Disconnect 均在同一线程执行；
 *   - 内置线程：调用 Start() 派生一个 io 线程；此时从其它线程调用
 *     Send / Disconnect 会被自动 post 到 io 线程串行执行（Send 返回 Ok
 *     表示已入队，非最终发送结果）。
 * 禁止在回调内析构 OrkClient 或调用 Stop()（会 join 自身线程 → 死锁）。
 */

#include "ork/client/client_config.hpp"
#include "ork/codec/bootstrap_frame.hpp"
#include "ork/crypto/ork_cipher_types.hpp"
#include "ork/platform/data/send_result.hpp"
#include "ork/platform/data/transport_channel.hpp"
#include "ork/platform/utils/secure_random.hpp"
#include "ork/transport/transport_kcp/kcp_session.hpp"
#include "ork/transport/transport_udp/udp_packet_header.hpp"

#include <asio.hpp>

#if defined(ORK_HAVE_TLS)
#include <asio/ssl.hpp>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace ork::client {

/// 发送结果（重导出 platform 层枚举，保持 SDK 语义一致）
using SendResult = ork::platform::SendResult;

/**
 * @brief 客户端事件回调接口（业务方实现）
 *
 * 所有回调均在驱动线程（PumpOnce 调用线程或 Start() 派生的 io 线程）内触发，
 * 实现须非阻塞。payload 生命周期仅限回调内有效。
 */
class IClientListener
{
public:
    virtual ~IClientListener() = default;

    /// 连接就绪（TCP bootstrap + UDP 握手 + KCP ACK 完成）
    virtual void OnConnected(uint32_t connectionId) { (void)connectionId; }

    /// 连接断开或连接失败。reason 见 DisconnectReason 语义（0=正常/本地关闭）
    virtual void OnDisconnected(uint32_t reason) { (void)reason; }

    /// 收到 opaque 载荷（core 不解释内容）
    virtual void OnPayload(std::span<const uint8_t> data, TransportChannel channel)
    {
        (void)data;
        (void)channel;
    }
};

/// 客户端连接状态机
enum class ClientState : uint8_t
{
    Disconnected,   ///< 未连接
    Connecting,     ///< TCP 连接中
    TlsHandshaking, ///< A1.1：TLS 握手中（TlsServerAuthenticated 模式）
    Bootstrapping,  ///< 等待接收 BootstrapFrame（TLS 模式下在 TLS 通道内）
    Handshaking,    ///< UDP 握手 + 等待 KCP ACK
    Connected,      ///< 已就绪，可收发
    Disconnecting   ///< 关闭中
};

/// 状态可读名（日志用）
constexpr const char* ToString(ClientState s) noexcept
{
    switch (s)
    {
    case ClientState::Disconnected:   return "Disconnected";
    case ClientState::Connecting:     return "Connecting";
    case ClientState::TlsHandshaking: return "TlsHandshaking";
    case ClientState::Bootstrapping:  return "Bootstrapping";
    case ClientState::Handshaking:    return "Handshaking";
    case ClientState::Connected:      return "Connected";
    case ClientState::Disconnecting:  return "Disconnecting";
    }
    return "Unknown";
}

/**
 * @brief Ork 客户端主类
 *
 * 生命周期：
 * @code
 *   asio::io_context io;
 *   OrkClient client(io, config);
 *   client.Connect(&listener);
 *   // 手动驱动：
 *   while (running) { client.PumpOnce(); }
 *   // 或内置线程：client.Start(); ... client.Stop();
 * @endcode
 */
class OrkClient
{
public:
    OrkClient(asio::io_context& io, const ClientConfig& config);
    ~OrkClient();

    OrkClient(const OrkClient&) = delete;
    OrkClient& operator=(const OrkClient&) = delete;

    // ---- 生命周期 ----

    /// 异步启动连接流程（TCP → bootstrap → UDP handshake → KCP）
    void Connect(IClientListener* listener);

    /// 主动断开（关闭 socket、销毁 KCP、触发 OnDisconnected）
    void Disconnect();

    /// 派生内置 io 线程驱动 io_context（与 PumpOnce 二选一）
    void Start();

    /// 停止内置 io 线程（reset work guard + join）
    void Stop();

    /// 手动驱动一次：运行所有就绪的异步回调（非阻塞）
    void PumpOnce();

    // ---- 发送 ----

    /// 发送 opaque 载荷。Reliable → KCP，Unreliable → UDP。
    SendResult Send(std::span<const uint8_t> payload, TransportChannel channel);

    // ---- 状态查询 ----

    ClientState GetState() const { return state_; }
    uint32_t GetConnectionId() const { return connectionId_; }

    /**
     * @brief Bootstrap V2 下发的安全域标识（Rev 3 §5.4）
     *
     * 每次成功 Bootstrap 后保存；B1（UDP Handshake V2）落地后作为
     * authTag 计算的必含输入。安全敏感值：不得写入日志。
     */
    const ork::platform::BootstrapEpoch& GetBootstrapEpoch() const { return bootstrapEpoch_; }

private:
    // 连接流程
    void DoTcpConnect();
    void OnTcpConnected(const asio::error_code& ec);
#if defined(ORK_HAVE_TLS)
    void DoTlsHandshake();
    void OnTlsHandshakeComplete(const asio::error_code& ec);
    void ArmTlsHandshakeTimeout();
    void OnTlsHandshakeTimeout(const asio::error_code& ec);
#endif
    void OnBootstrapLength(const asio::error_code& ec);
    void OnBootstrapReceived(const asio::error_code& ec);
    void ApplyBootstrap(const uint8_t* data, size_t len);
    void DoUdpHandshake();
    /// B1：构造并发送 42B Handshake V2（当前 nonce/counter；溢出 fail closed）
    void SendHandshakeV2();
    /// B1：ACK 等待重试定时器（同 nonce、counter 严格递增）
    void ArmHandshakeRetry();
    void OnHandshakeRetry(const asio::error_code& ec);
    void StartKcpSession();
    void OnUdpHandshakeAck();
    void FailConnection(uint32_t reason);

    // 数据收发
    void DoUdpReceive();
    void OnUdpReceive(const asio::error_code& ec, size_t bytes);
    bool IsUdpDataPacket(const uint8_t* data, size_t len) const;
    void ProcessKcpInbound();
    void ProcessUdpInbound(const uint8_t* data, size_t len);

    // KCP pump
    void ScheduleKcpUpdate();
    void OnKcpUpdate(const asio::error_code& ec);

    // 超时
    void ArmConnectTimeout();
    void OnConnectTimeout(const asio::error_code& ec);

    // 发送实现（驱动线程内）
    SendResult SendReliable(std::span<const uint8_t> payload);
    SendResult SendUnreliable(std::span<const uint8_t> payload);
    SendResult SendImpl(std::span<const uint8_t> payload, TransportChannel channel);

    void DisconnectImpl();
    bool RunningInIoThread() const;

    // ---- 成员 ----
    asio::io_context& io_;
    ClientConfig config_;
    ClientState state_ = ClientState::Disconnected;
    IClientListener* listener_ = nullptr;

    // 解析得到的服务端地址（TCP/UDP 共用）
    asio::ip::address serverAddr_;

    // TCP
    asio::ip::tcp::socket tcpSocket_;
#if defined(ORK_HAVE_TLS)
    /// A1.1：TLS 模式（非空 ctx = TlsServerAuthenticated）；生命周期覆盖 stream
    std::shared_ptr<asio::ssl::context> tlsContext_;
    /// TLS 模式：socket move 进 ssl stream（此后 TCP IO 均走 TLS）
    std::optional<asio::ssl::stream<asio::ip::tcp::socket>> tlsStream_;
    /// TLS 握手超时（独立于 connectTimeoutMs 总超时）
    asio::steady_timer tlsHandshakeTimer_;
    bool tlsHandshakeTimedOut_ = false;
#endif
    std::array<uint8_t, 4> tcpLenBuf_{};
    std::vector<uint8_t> tcpBodyBuf_;
    uint32_t bootstrapLen_ = 0;

    // UDP
    asio::ip::udp::socket udpSocket_;
    asio::ip::udp::endpoint serverEndpoint_;
    asio::ip::udp::endpoint remoteEndpoint_;
    std::array<uint8_t, 1500> recvBuffer_{};

    // Session
    uint32_t connectionId_ = 0;
    uint32_t sessionToken_ = 0;
    /// Bootstrap V2 安全域标识（每次成功 Bootstrap 后保存；§5.4；不落日志）
    ork::platform::BootstrapEpoch bootstrapEpoch_{};

    // B1：UDP Handshake V2 握手材料（每次成功 Bootstrap 后重置；不落日志）
    /// 当前握手 clientNonce（16B CSPRNG；同一次连接内重试不变）
    ork::platform::ClientNonce clientNonce_{};
    /// 已发送的最大 counter（严格递增；溢出 fail closed，不回绕）
    uint32_t handshakeCounter_ = 0;

    // KCP
    std::unique_ptr<KcpSession> kcpSession_;
    asio::steady_timer kcpTimer_;
    std::vector<uint8_t> kcpRecvBuf_;

    // Crypto（OrkCipher 为纯静态方法；此处持有每连接状态）
    ork::crypto::CipherState cipherState_;

    // 超时
    asio::steady_timer connectTimer_;
    /// B1：握手 ACK 等待重试定时器（间隔 udpHandshakeRetryIntervalMs）
    asio::steady_timer handshakeRetryTimer_;

    // 构造体初始化（tlsHandshakeTimer_ 依赖 io_；保留在成员初始化列表）

    // IO thread（可选）
    std::thread ioThread_;
    std::atomic<bool> ioThreadRunning_{false};
    std::thread::id ioThreadId_{};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard_;
};

}  // namespace ork::client
