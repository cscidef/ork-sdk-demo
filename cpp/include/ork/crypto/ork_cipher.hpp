#pragma once
/**
 * @file ork_cipher.hpp
 * @brief Ork Cipher v1 - ChaCha20-Poly1305 AEAD transport encryption
 *
 * @see OrkTransportCipher_v1.md for full specification
 * @status Phase 1: MakeNonce / MakeAad / Seal / Open / replay implemented
 */

#include "ork/crypto/ork_cipher_types.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ork::crypto {

/**
 * @brief Open 失败原因分类（Phase 4.5：供传输层加密失败计数）
 *
 * Open 返回 nullopt 时，若调用方传入 out_reason，则写入对应分类；
 * 成功或 passthrough 时写 None。不改变 Open 的返回值语义。
 */
enum class OpenFailReason : uint8_t {
    None = 0,      ///< 成功（或明文 passthrough）
    Expired,       ///< 密钥过期
    Malformed,     ///< 帧格式非法（太短 / ver 错 / 长度不一致 / packet_type 不匹配）
    Replay,        ///< 重放拒绝（cheap filter / KCP seq 回退）
    Aead,          ///< AEAD 验签失败
};

/**
 * @brief OrkCipher provides Seal/Open operations for transport layer encryption
 *
 * Phase 1: AEAD Seal/Open, nonce, AAD, KCP/UDP replay（规范 §9–§11 / §15）。
 * Phase 1b: 挂接 network_transport；帧头含明文 packet_type（15B 头，见 §9 注）。
 *
 * @note 返回值约定（与规范 §16 略有扩展）：
 *       - Seal / Open 均使用 std::optional：规范 §16 中 Seal 为 vector、Open 为 optional；
 *         此处 Seal 也用 optional，以便 AEAD 失败时返回 nullopt，与 Open 对称。
 *       - encryptionRequired=false 时两者均 passthrough 返回字节（非 nullopt），见 §15.2 / §15.3。
 *
 * @note 调用示例：
 * @code
 *   // 客户端发 KCP 可靠指令（C2S）
 *   auto frame = OrkCipher::Seal(state, CipherChannel::Kcp,
 *       CipherDirection::ClientToServer, msgType, pbBytes);
 *
 *   // 服务端收同一包（对端方向仍为 C2S；packet_type=0 从帧头读取，见 Open 说明）
 *   auto plain = OrkCipher::Open(state, CipherChannel::Kcp,
 *       CipherDirection::ClientToServer, 0, *frame);
 * @endcode
 */
class OrkCipher {
public:
    /// OrkCipherFrame 头部长度：ver(1)+seq(8)+payload_len(2)+packet_type(4)
    static constexpr size_t kFrameHeaderSize = 15;

    /**
     * @brief 初始化新连接的 CipherState（Bootstrap 时调用）
     *
     * 生成随机 c2s/s2c key，设置 connection/token/过期时间与 encryptionRequired。
     */
    static void InitConnectionCipher(
        CipherState& state,
        uint32_t connection_id,
        uint32_t session_token,
        bool encryption_required,
        int64_t expire_at_ms);

    /// 开启后 Seal/Open 经 Logger::Debug 打印帧元数据（conn/ch/seq/长度与失败原因）；
    /// 不输出 token、密钥材料或载荷/帧字节 hex（联调验收用，生产应关闭）
    static void SetDebugLogging(bool enabled);
    static bool IsDebugLogging();

    /**
     * @brief Seal plaintext into OrkCipherFrame (AEAD encrypt)
     *
     * @param state  Per-connection cipher state; txSeq is incremented on encrypt.
     * @param channel  KCP or UDP — selects state.kcp vs state.udp for seq/replay.
     * @param direction  Local sending direction: C2S uses c2sKey, S2C uses s2cKey.
     * @param packet_type  NexusMessage MessageType enum value; bound into AAD (§9).
     * @param plaintext  Protobuf serialized payload (before framing).
     *
     * @return OrkCipherFrame bytes on success.
     *         If state.encryptionRequired == false: passthrough plaintext unchanged (§15.2).
     *         std::nullopt only on AEAD / framing failure (not when encryption is off).
     *
     * @note 规范 §15.2：关加密时返回 plain，不是 nullopt。
     */
    static std::optional<std::vector<uint8_t>> Seal(
        CipherState& state,
        CipherChannel channel,
        CipherDirection direction,
        uint32_t packet_type,
        std::span<const uint8_t> plaintext);

    /**
     * @brief Open OrkCipherFrame to plaintext (AEAD decrypt)
     *
     * @param state  Per-connection cipher state; replay counters updated only after AEAD success (§11).
     * @param channel  KCP or UDP — KCP uses state.kcp.kcpRx, UDP uses state.udp.udpRx.
     * @param peer_direction  Peer's sending direction (selects key: peer C2S → c2sKey, peer S2C → s2cKey).
     * @param packet_type  加密开启时：0 = 从帧头 offset 11 读取（Phase 1b 传输层默认）；
     *                     非 0 = 须与帧内 packet_type 一致（用于单测显式校验）。
     * @param frame  Raw OrkCipherFrame bytes (or plaintext when encryption is off).
     *
     * @return Decrypted plaintext on success.
     *         If state.encryptionRequired == false: passthrough frame unchanged (§15.3).
     *         std::nullopt on AEAD failure, replay reject, expiry, or malformed frame.
     *
     * @note packet_type 语义（规范 §15.3 / §16，Phase 1b）：
     *       - **传输层默认传 0**：从帧头 offset 11 读取 `packet_type` 再构造 AAD（`network_transport` 即如此）。
     *       - **非 0**：显式指定 AAD 中的 `packet_type`，且须与帧头一致，否则立即失败（单测防挪包用）。
     *       - AAD 中的 `packet_type` 必须与 Seal 侧一致，否则 AEAD 验签失败；传 0 时以帧头为准，效果与显式传枚举值相同。
     *       - `encryptionRequired=false` 时忽略该参数，原样 passthrough。
     */
    [[nodiscard]] static std::optional<std::vector<uint8_t>> Open(
        CipherState& state,
        CipherChannel channel,
        CipherDirection peer_direction,
        uint32_t packet_type,
        std::span<const uint8_t> frame,
        OpenFailReason* out_reason = nullptr);
};

} // namespace ork::crypto
