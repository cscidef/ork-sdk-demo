#pragma once
/**
 * @file ork_cipher_types.hpp
 * @brief Ork Cipher v1 — 数据类型定义
 *
 * 本文件定义每连接加解密运行时状态，供 OrkCipher::Seal / Open 使用。
 *
 * @see OrkTransportCipher_v1.md §12 CipherState 数据结构
 *
 * @note 设计要点（规范 §12）：
 *       - KCP / UDP **各自独立** txSeq 与接收侧重放状态
 *       - c2sKey / s2cKey **双向共用**（不按 channel 再分 key）
 *       - nonce / AAD 内通过 channel_id 区分 KCP 与 UDP
 */

#include <array>
#include <bitset>
#include <cstdint>

namespace ork::crypto {

/**
 * @brief 通道标识（写入 nonce 与 AAD，区分 KCP 可靠通道与 UDP 不可靠通道）
 *
 * 与规范 §10 nonce 布局、§9 AAD 中的 channel_id 对应。
 */
enum class CipherChannel : uint8_t {
    Kcp = 0x01,  ///< KCP 可靠通道（ikcp 应用层 payload）
    Udp = 0x02   ///< UDP 不可靠通道（UdpDataHeader 后的 body）
};

/**
 * @brief 方向标识（选 key 与构造 nonce 时使用）
 *
 * | 方向 | 发送方 Seal 用 key | 接收方 Open 对端包用 key |
 * |------|-------------------|-------------------------|
 * | ClientToServer (C2S) | c2sKey | 对端 C2S → c2sKey |
 * | ServerToClient (S2C) | s2cKey | 对端 S2C → s2cKey |
 */
enum class CipherDirection : uint8_t {
    ClientToServer = 0x00,  ///< 客户端 → 服务端
    ServerToClient = 0x01   ///< 服务端 → 客户端
};

/**
 * @brief KCP 接收侧重放状态（极简单调 seq 检查，规范 §11）
 *
 * KCP 本身保证有序，接收侧只需拒绝 seq <= lastAcceptedSeq 的包。
 * 仅在 AEAD 解密**成功之后**才更新 lastAcceptedSeq（§11 / §15.4 DoS 回归）。
 */
struct KcpRxState {
    uint64_t lastAcceptedSeq = 0;  ///< 已接受的最大 seq；0 表示尚未收到任何包
};

/**
 * @brief UDP 接收侧重放窗口（256 包滑动窗口，对齐 netcode.io，规范 §11）
 *
 * 不可靠 UDP 可能乱序/重复，用 highestSeq + seen bitset 做 cheap filter。
 * 同样：AEAD 失败不得 commit 窗口状态。
 */
struct UdpReplayWindow {
    uint64_t highestSeq = 0;       ///< 窗口内已见过的最大 seq
    std::bitset<256> seen{};       ///< 相对 highestSeq 的 256 包可见位图
};

/**
 * @brief 单通道（KCP 或 UDP）的 seq 与接收状态
 *
 * 每个 CipherState 内含 kcp / udp 两份，发送时按 channel 选取对应 txSeq。
 * 结构内同时含 kcpRx 与 udpRx，使用时按 channel 只访问对应字段（规范 §15.3）。
 */
struct ChannelCipherState {
    uint64_t txSeq = 1;            ///< 本通道下一包发送 seq（Seal 时 post-increment）
    KcpRxState kcpRx;              ///< KCP 接收重放（channel==Kcp 时使用）
    UdpReplayWindow udpRx;         ///< UDP 接收重放（channel==Udp 时使用）
};

/**
 * @brief 每逻辑连接的完整 Cipher 状态
 *
 * 在 TCP Bootstrap（AssignConnection）成功后由服务端下发并初始化；
 * 生命周期与 connection_id / session_token 绑定。
 *
 * @see OrkTransportCipher_v1.md §8 Bootstrap、§12
 *
 * @note 字段与 Bootstrap proto 对应关系（待 Proto 扩展）：
 *       - c2sKey / s2cKey  ← AssignConnection.c2s_key / s2c_key（各 32 字节随机）
 *       - connectionId     ← AssignConnection.connection_id
 *       - sessionToken     ← AssignConnection.session_token
 *       - expireAt         ← AssignConnection.expire_at（Unix ms）
 *       - encryptionRequired ← AssignConnection.encryption_required / config.enable_encryption
 */
struct CipherState {
    std::array<uint8_t, 32> c2sKey{};  ///< 客户端发送 / 服务端解 C2S 包
    std::array<uint8_t, 32> s2cKey{};  ///< 服务端发送 / 客户端解 S2C 包
    uint32_t connectionId = 0;         ///< 连接 ID，参与 AAD 绑定
    uint32_t sessionToken = 0;         ///< 会话令牌，参与 AAD 绑定
    int64_t expireAt = 0;              ///< 密钥过期时间（Unix 毫秒）；Open 时超时则拒绝
    bool encryptionRequired = false;   ///< false → Seal/Open passthrough 明文（§15.2/§15.3）

    ChannelCipherState kcp;            ///< KCP 通道独立 seq / replay
    ChannelCipherState udp;            ///< UDP 通道独立 seq / replay
};

} // namespace ork::crypto
