#pragma once
#include "ork/connection/connection_state.hpp"
#include "ork/crypto/ork_cipher_types.hpp"
#include "ork/transport/data/network_defs.hpp"
#include "ork/platform/time/time_defs.hpp"
#include "ork/platform/utils/secure_random.hpp"
#include "ork/transport/transport_tcp/tcp_connection.hpp"
#include <asio.hpp>
#include <memory>
#include <optional>

/** @brief 传输层逻辑连接（ConnectionManager 表项） */
struct Connection
{
    ConnectionId id;
    SessionToken sessionToken;

    /**
     * @brief Bootstrap V2 安全域标识（Rev 3 §5.4）：每次新连接创建时经
     *        CSPRNG 生成，随 Bootstrap V2 Payload 下发；B1 起为 authTag
     *        的必含输入（把握手材料唯一绑定到“这一次 Bootstrap”）。
     *        安全敏感值：不得写入日志、不得进快照（A0.1 §7.2 规则）。
     */
    ork::platform::BootstrapEpoch bootstrapEpoch{};

    /**
     * @brief B1：UDP Handshake V2 首次绑定的当前 clientNonce（Rev 3 §5.7）
     *
     * 首次合法绑定时原子提交（与 endpoint / highestCounter / Established 同一
     * NetworkPump strand 调用内写入）；此后同 nonce + 递增 counter 的重试仅
     * 推进 highestCounter，不同 nonce 一律拒绝（B2 才引入 challenge-response）。
     * 安全敏感值：不得写入日志、不得进快照。
     */
    std::optional<ork::platform::ClientNonce> udpHandshakeNonce{};

    /**
     * @brief B1：已接受的最大握手 counter（防重放水位）
     *
     * 同 nonce 且 counter 严格大于水位才接受（仅推进水位 + 重发 ACK）；
     * 相同/更小 counter 静默丢弃。与 udpHandshakeNonce 同点原子提交。
     */
    uint32_t udpHandshakeHighestCounter{0};

    // !FIXME: avoid direct access
    std::shared_ptr<TcpConnection> tcp;
    std::optional<asio::ip::udp::endpoint> udp;

    ConnectionState state;

    /** @brief 最后收到入站包的时间（单调时钟微秒），空闲超时判定用；TouchActivity 刷新 */
    TimePoint lastActive;

    /**
     * @brief A0.2：进入 TcpOnly（未完成 Bootstrap）的时刻（单调时钟微秒）。
     *
     * TcpOnly 停留超时（资源保护 tcp_only_timeout_sec）判定用；
     * OnTcpConnected / RebindTcp 进入 TcpOnly 时刷新，Established 后失效。
     */
    TimePoint tcpOnlySince{0};

    /// Per-connection Ork Cipher state (keys / seq / replay)
    ork::crypto::CipherState cipher{};
};
