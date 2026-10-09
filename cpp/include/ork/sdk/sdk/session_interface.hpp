/**
 * @file session_interface.hpp
 * @brief Ork SDK 核心公开接口（Phase 3.4：纯传输职责）
 *
 * core 只保留：连接、传输、opaque payload、网络统计。
 * 玩家身份、房间状态、游戏 Tick、Ack 等业务语义全部下沉到宿主（examples）：
 * - 事件入口：ork::sdk::IHostListener（OnConnected/OnDisconnected/OnPayload/OnStats）
 * - 发送能力：ork::sdk::ISender（Send/Broadcast，opaque 字节）
 * - 连接治理：IConnectionRegistry（Count/TryGet/ForEach/Snapshot/Disconnect）
 * - 传输配置：TransportConfig（网络、加密、bootstrap、空闲、pump/timing）
 */

#pragma once

#include "ork/platform/types.hpp"
#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/platform/data/disconnect_reason.hpp"
#include "ork/platform/data/transport_channel.hpp"
#include "ork/platform/data/kcp_config.hpp"
#include "ork/transport/transport_tcp/connection_rate_limiter.hpp"
#include "ork/sdk/host_listener.hpp"
#include "ork/sdk/sender.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ConnectionId / Port 来自 ork/platform/types.hpp

/**
 * @brief 传输层链路状态（TCP/UDP/KCP）
 *
 * 来自 ConnectionManager / Connection::state 的映射：
 * - Disconnected：TCP 已断
 * - TcpOnly：TCP bootstrap 完成，UDP 未就绪
 * - Established：TCP + UDP（+ KCP）就绪
 */
enum class TransportLinkState
{
    Disconnected,
    TcpOnly,
    Established
};

/**
 * @brief 单条连接只读快照（IConnectionRegistry 查询结果）
 *
 * 仅含传输字段；会话/玩家状态由宿主在 OnPayload 之上自行维护。
 */
struct ConnectionView
{
    ConnectionId connectionId = 0;
    TransportLinkState linkState = TransportLinkState::Disconnected;
    uint64_t lastActiveUs = 0;      ///< 最后收包时间（单调时钟微秒）
    bool encryptionActive = false;  ///< 是否启用 OrkCipher
};

/**
 * @brief 服务端连接注册表（查询、遍历、断开）
 *
 * 通过 SessionFramework::GetConnectionRegistry() 获取。
 * Phase 3.4 收窄为五个方法；按 playerId/uuid 的反查属业务语义，由宿主自建表。
 */
class IConnectionRegistry
{
public:
    virtual ~IConnectionRegistry() = default;

    /** @brief 活跃连接数（linkState != Disconnected） */
    virtual size_t Count() const = 0;

    /** @brief 查询单条连接；不存在返回 false */
    virtual bool TryGet(ConnectionId connectionId, ConnectionView& out) const = 0;

    /** @brief 遍历所有活跃连接（跳过 linkState==Disconnected） */
    virtual void ForEach(const std::function<void(const ConnectionView&)>& fn) const = 0;

    /**
     * @brief 全量只读快照（值语义）
     *
     * 当前实现为遍历传输层连接表逐条拷贝，期间表可能被 IO 线程并发修改，
     * 返回的是"逐条一致的近似"而非原子快照；不承诺跨条目一致。
     * Phase 4 引入 strand 后将升级为强一致快照。
     */
    virtual std::vector<ConnectionView> Snapshot() const = 0;

    /**
     * @brief 主动断开指定连接
     * @return 连接存在且非 Disconnected 时 true
     * @note 可在 IHostListener 回调线程调用（内部为异步 socket close，非阻塞）；
     *       禁止在回调中调用 SessionFramework::Stop()（见 host_listener.hpp 线程契约）
     */
    virtual bool Disconnect(ConnectionId connectionId,
                            DisconnectReason reason = DisconnectReason::Kicked) = 0;
};

// ---- TransportConfig ----

/**
 * @brief 传输配置（core 唯一配置面）
 *
 * 仅网络、加密、bootstrap、空闲超时、pump/timing；房间/tick/delta 等
 * 业务配置由宿主自持（examples 参见 RoomConfig）。
 */
struct TransportConfig
{
    /** @brief 游戏端口（TCP bootstrap 与 UDP 共用）；Create() 必填，0 表示无效 */
    Port gamePort = 0;

    /** @brief 是否启用 KCP 可靠传输 */
    bool enableKcp = false;
    /**
     * @brief KCP 更新频率（Hz）。
     *
     * Phase 4.3：驱动 NetworkPump strand timer（拍间隔 = 1000/kcpUpdateRate ms），
     * 每拍执行 KCP Update / Recv。默认 100Hz=10ms。
     * 约束：必须 > 0 且 >= pumpRate（否则 SessionFramework::Create 抛异常）。
     */
    uint32_t kcpUpdateRate = 100;

    /** @brief 连接空闲超时（秒）。超过此时间无入站包则断开；0=禁用 */
    uint32_t connectionIdleTimeoutSec = 0;

    /** @brief OrkCipher：Bootstrap 后 KCP/UDP 是否走 AEAD 加密 */
    bool enableEncryption = false;
    /** @brief AEAD 密钥有效期（秒），仅 enableEncryption=true 时生效 */
    uint32_t bootstrapTtlSec = 300;
    /** @brief 加解密调试日志（生产环境应 false） */
    bool cipherDebugLog = false;

    /**
     * @brief 宿主 pump 频率（Hz）。
     *
     * Phase 4.3：core 在 strand 内经累加器分频，以本频率驱动宿主层工作：
     * 入站消息分发（IHostListener）、连接快照发布、空闲检测、OnStats 上报。
     * 与 kcpUpdateRate 关系：kcpUpdateRate >= pumpRate，每 (kcpUpdateRate/pumpRate)
     * 个 KCP 拍触发一次宿主 pump（如 kcp=100Hz, pump=50Hz → 每 2 拍一次）。
     * 与游戏 Tick 概念无关。
     */
    uint32_t pumpRate = 30;
    /** @brief pump 单帧上限（ms）；0 时按 pumpRate 推算 ceil(1000/pumpRate) */
    uint32_t pumpMaxFrameTimeMs = 0;

    /**
     * @brief KCP 协议参数（Phase 4.1）
     *
     * 默认值严格等于 Phase 2 硬编码。
     * 由 NetworkConfig YAML 解析后填充，传递给 NetworkTransport → KcpSession。
     */
    ork::transport::KcpConfig kcpConfig;

    /**
     * @brief 共享单包 payload 上限（字节），权威来源为 NetworkConfig::max_packet_size。
     *
     * Phase 4.1：同时作为 KCP MTU（ikcp_setmtu）与 Sender payload 门禁上限
     * （Reliable 与 Unreliable 两通道共享同一个值），避免两字段漂移。
     *
     * 默认 kDefaultMaxPacketSize(1200)（安全默认；嵌入用户未走 YAML 时亦不会回退 65535）。
     * 生产路径由 NetworkConfig YAML 解析后填充（范围 [kKcpMtuMin, kKcpMtuMax]）。
     * SessionFramework::Create() 校验范围合法性。
     */
    int maxPacketSize = ork::transport::kDefaultMaxPacketSize;

    /**
     * @brief UDP socket 接收/发送内核缓冲区字节数（Phase 4.4）
     *
     * 0 = 沿用 OS 默认（不显式 setsockopt）；>0 = 请求值（OS 可能 clamp，
     * 实际生效值由 UdpTransportServer::Start() 读取并写日志）。
     * 权威来源为 NetworkConfig::udp_receive_buffer_bytes / udp_send_buffer_bytes。
     */
    int udpReceiveBufferBytes = 0;
    int udpSendBufferBytes = 0;

    /**
     * @brief A0.2：Bootstrap 资源保护（连接接纳门禁 + TcpOnly 超时回收）
     *
     * 四层判定（TcpTransportServer::DoAccept 的 accept 完成回调，strand 内）：
     * TCP 活跃总上限 → 未完成 Bootstrap（TcpOnly）上限 → per-IP token
     * bucket → 全局 token bucket；拒绝静默。权威来源为 NetworkConfig 的
     * tcp_max_active_connections 等 7 个扁平字段。
     */
    ork::transport::ConnectionProtectionConfig connectionProtection{};

    // ---- A1.1：Bootstrap 通道安全（TLS 1.3 / 明文开发模式） ----

    /**
     * @brief Bootstrap 通道安全模式；默认 TlsServerAuthenticated（生产安全默认）
     *
     * - TlsServerAuthenticated：TLS 1.3 only、0-RTT 禁用；须提供
     *   tlsCertificateFile / tlsPrivateKeyFile。Create() fail closed（路径为空、
     *   文件缺失、私钥权限非 owner-only 均拒绝）；accept 后先 TLS 握手、
     *   成功才上报连接并发送 Bootstrap V2（109B 原样进 TLS 通道）。
     *   证书有效期验证依赖系统时钟（启动日志含诊断提示）。
     * - PlaintextDevelopment：仅 -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 构建编入；
     *   服务端强制 loopback bind，未编入时 Create() 拒绝。无 TLS 失败回退。
     */
    ork::platform::BootstrapSecurityMode bootstrapSecurity =
        ork::platform::BootstrapSecurityMode::TlsServerAuthenticated;

    /** @brief TLS 模式：PEM 证书链路径（leaf 在前，含中间链） */
    std::string tlsCertificateFile;

    /** @brief TLS 模式：PEM 私钥路径（加载前 POSIX owner-only 权限检查） */
    std::string tlsPrivateKeyFile;

    /** @brief TLS 握手超时（ms）；默认 5000。握手中连接纳入 pendingTls 预算 */
    uint32_t tlsHandshakeTimeoutMs = 5000;
};

// ---- SessionFramework：SDK 入口 ----

/**
 * @brief 会话框架（SDK 核心类，纯传输职责）
 *
 * 提供能力：
 * - 事件回调（IHostListener：连接 / 断开 / opaque 载荷 / 统计）
 * - 消息发送（ork::sdk::ISender：Send / Broadcast）
 * - 连接治理（IConnectionRegistry：查询 / 遍历 / 快照 / 断开）
 * - 内部 strand 调度（Phase 4.3：kcpUpdateRate 驱动 KCP，pumpRate 分频驱动宿主回调）
 *
 * 使用流程：
 * 1. SessionFramework::Create(config) 创建实例（config.gamePort 必填）
 * 2. RegisterListener(listener)
 * 3. Start() 启动传输与 pump；Stop() 逆序清理
 */
class SessionFramework
{
public:
    /**
     * @brief 创建会话框架实例
     * @param config 传输配置
     * @return 框架实例
     */
    static std::unique_ptr<SessionFramework> Create(const TransportConfig& config);

    virtual ~SessionFramework() = default;

    /**
     * @brief 注册宿主监听器
     *
     * 必须在 Start() 之前调用；nullptr 清除监听。
     * 线程契约见 ork/sdk/host_listener.hpp。
     */
    virtual void RegisterListener(ork::sdk::IHostListener* listener) = 0;

    /** @brief 启动会话框架 */
    virtual void Start() = 0;

    /** @brief 停止会话框架（不得在网络 pump 线程内调用） */
    virtual void Stop() = 0;

    /** @brief 获取 opaque 发送器（Send / Broadcast） */
    virtual ork::sdk::ISender& GetSender() = 0;

    /**
     * @brief 获取连接注册表（查询连接状态、断开等）
     *
     * SessionFrameworkImpl 自身实现 IConnectionRegistry，返回 *this。
     */
    virtual IConnectionRegistry& GetConnectionRegistry() = 0;

    /** @brief 获取传输配置 */
    virtual const TransportConfig& GetConfig() const = 0;
};
