#pragma once
/**
 * @file client_config.hpp
 * @brief OrkClient 客户端配置（Phase 5C.1 / A1.1 Bootstrap Security）
 *
 * 客户端连接参数：服务端地址、TCP/UDP 端口、KCP 与加密开关、共享包长上限。
 * KcpConfig 复用 platform 层值类型（ork/platform/data/kcp_config.hpp），
 * 与服务端 NetworkConfig 对齐，避免参数漂移。
 *
 * A1.1：Bootstrap 通道安全字段（TLS 1.3 / 明文开发模式），与服务端
 * BootstrapSecurityMode 对齐；TLS 失败无明文回退。
 */

#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/platform/data/kcp_config.hpp"

#include <cstdint>
#include <string>

namespace ork::client {

/**
 * @brief OrkClient 连接与传输配置
 *
 * @note 服务端 TCP bootstrap 与 UDP/KCP 通常监听同一 gamePort；
 *       tcpPort / udpPort 分列以兼容端口分离部署，默认各自独立。
 * @note enableEncryption 仅作客户端意向；实际是否加密以服务端下发的
 *       BootstrapFrame.flags（bit0 encryption_required）为权威。
 */
struct ClientConfig
{
    /// 服务端主机（IP 或可解析域名）
    std::string host = "127.0.0.1";
    /// TCP bootstrap 端口
    uint16_t tcpPort = 9400;
    /// UDP / KCP 数据端口
    uint16_t udpPort = 9401;
    /// 是否启用 KCP 可靠通道（false 时 Reliable 通道不可用）
    bool enableKcp = true;
    /// 客户端加密意向（权威以 BootstrapFrame.flags 为准）
    bool enableEncryption = true;
    /// 共享单包 payload 上限（字节），同时作为 KCP MTU；范围 [kKcpMtuMin, kKcpMtuMax]
    uint32_t maxPacketSize = 1200;
    /// KCP 协议参数（复用 platform 层值类型）
    ork::transport::KcpConfig kcpConfig;
    /// 连接 + 握手整体超时（ms）；超时未 Connected 则触发 OnDisconnected
    uint32_t connectTimeoutMs = 5000;

    // ---- B1：UDP Handshake V2 重试 ----

    /**
     * @brief 握手 ACK 等待重试间隔（ms）；0 = 禁用自动重试（仅总超时兑底）
     *
     * 重试复用同一 clientNonce、counter 严格递增（0->1->2...，不回绕；
     * Rev 3 §5.6/§5.8），由服务端幂等状态机收敛为重发 ACK；
     * counter 耗尽（UINT32_MAX）即 fail closed 断开，须重连重走 Bootstrap。
     */
    uint32_t udpHandshakeRetryIntervalMs = 500;

    // ---- A1.1：Bootstrap 通道安全 ----

    /**
     * @brief Bootstrap 通道安全模式；默认 TlsServerAuthenticated（生产安全默认）
     *
     * - TlsServerAuthenticated：TCP connect 后先做 TLS 1.3 握手（verify_peer +
     *   SNI + SAN/hostname 验证），成功才读 4B prefix 与 109B Bootstrap V2。
     *   失败即断开，无明文回退。
     * - PlaintextDevelopment：仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 构建可用，
     *   且连接目标必须为 loopback（否则拒绝并输出醒目警告）。
     */
    ork::platform::BootstrapSecurityMode securityMode =
        ork::platform::BootstrapSecurityMode::TlsServerAuthenticated;

    /**
     * @brief TLS 模式：受信 CA PEM 文件路径（空 = 仅系统 trust store）
     *
     * 非空时作为受信锚加载（SSL_CTX_load_verify_locations）；与
     * useSystemTrustStore 按明确规则叠加（见 ClientConfig 组合语义：
     * caFile 空且 useSystemTrustStore=false = 无信任锚，拒绝连接 fail closed）。
     */
    std::string caFile;

    /**
     * @brief TLS 模式：期望服务端名（必填非空）
     *
     * 同时用于 SNI（SSL_set_tlsext_host_name）与 SAN/hostname 验证
     * （X509_VERIFY_PARAM_set1_host；证书含 SAN 时只验 SAN）。
     * 典型值：服务端证书 SAN 中的 DNS 名（如 "ork.internal"）。
     */
    std::string expectedServerName;

    /// TLS 模式：叠加系统默认信任路径（SSL_CTX_set_default_verify_paths）；默认 true
    bool useSystemTrustStore = true;

    /// TLS 握手超时（ms）；默认 5000。超时即断开（无回退）
    uint32_t tlsHandshakeTimeoutMs = 5000;
};

}  // namespace ork::client
