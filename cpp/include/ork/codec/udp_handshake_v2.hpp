#pragma once
/**
 * @file udp_handshake_v2.hpp
 * @brief UDP Handshake V2（42B）编解码与认证 — Bootstrap Security B1（Rev 3 §5.5/§5.6）
 *
 * 取代 V1 9B UdpHandshakeHeader（type + connectionId + sessionToken）：
 * 首次 UDP 绑定必须经 HMAC-SHA-256 认证，无 sessionToken 上线传输。
 *
 * 帧布局（Rev 3 §5.5 冻结，总长 42B = 1+1+4+16+4+16）：
 *
 *   +---------+---------+----------------+-------------+----------+----------+
 *   | type    | version | connectionId   | clientNonce | counter  | authTag  |
 *   | 1B=0x03 | 1B=0x02 | 4B BE          | 16B         | 4B BE    | 16B      |
 *   +---------+---------+----------------+-------------+----------+----------+
 *
 * 首字节为路由 type（UdpPacketType::HandshakeV2 = 0x03），version 退居第二字节
 * （避免与 Data=2 冲突；后续演进递增 version，type 不变）。
 *
 * 密钥与认证（Rev 3 §5.6 冻结）：
 *   - handshakeKey = HKDF-SHA-256(ikm = c2sKey, salt = 32×0x00,
 *                                  info = "ORK-UDP-HS-C2S-v2", L = 32)；
 *     不复用数据通道 nonce/密钥；key 只驻留内存，不日志、不序列化；
 *   - authTag = HMAC-SHA-256(handshakeKey, macInput) 截断前 16B；
 *   - macInput（62B，固定拼接顺序）= ASCII "ORK-UDP-HANDSHAKE-V2"(20B)
 *     || type(1) || version(1) || connectionId(4 BE) || bootstrapEpoch(16)
 *     || clientNonce(16) || counter(4 BE)；
 *   - 验证使用常量时间比较（libsodium crypto_verify_16）。
 *
 * 线路编码规则（与 BootstrapFrame 一致，§5.2 强制）：
 *   - 逐字段写入 / 逐字段读取，禁止整块 memcpy 依赖编译器布局；
 *   - 全部常量与偏移只引用 bootstrap_protocol_constants.hpp（§14.1 唯一权威）。
 *
 * 服务端消费顺序（Rev 3 §5.7）：结构解码（本文件，无 HMAC）→ connectionId
 * 查表（未命中静默丢弃，不做 HMAC）→ 有效期检查 → authTag 常量时间验证
 * → 防重放状态机 → strand 内原子提交（ConnectionManager / NetworkTransport）。
 */

#include "ork/codec/bootstrap_protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ork::codec {

/// Handshake V2 解码失败原因（§5.7 步骤 [1]；首个失败即停，不含 HMAC 结果）
enum class HandshakeV2DecodeError : uint8_t
{
    None = 0,
    /// 帧字节数 ≠ 42（截断 / 超长）
    BadFrameSize,
    /// type ≠ kUdpHandshakeV2PacketType（含 V1 0x00 嗅探形态）
    BadType,
    /// version ≠ kUdpHandshakeV2Version
    UnsupportedVersion,
    /// authTag 常量时间比较失败（VerifyHandshakeV2AuthTag 专用）
    BadAuthTag,
};

/// 错误可读名（日志 / 测试断言用；不含任何字段值）
const char* ToString(HandshakeV2DecodeError error) noexcept;

/**
 * @brief Handshake V2 帧字段值对象（authTag 外的 MAC 输入域字段）
 */
struct UdpHandshakeV2Frame
{
    static constexpr size_t kSize = kUdpHandshakeV2Size;  ///< 42

    uint32_t connectionId = 0;  ///< 4B BE（公开可预测路由标识，非秘密）
    std::array<uint8_t, kClientNonceSize> clientNonce{};  ///< 16B CSPRNG
    uint32_t counter = 0;  ///< 4B BE（同 nonce 下重试单调计数）
};

/// counter 是否还能递增（溢出即 fail closed / 重连，禁止回绕；Rev 3 §5.6）
[[nodiscard]] inline constexpr bool HandshakeCounterCanAdvance(uint32_t counter) noexcept
{
    return counter != std::numeric_limits<uint32_t>::max();
}

/**
 * @brief HKDF-SHA-256 派生 handshakeKey（Rev 3 §5.6 冻结参数）
 *
 *   PRK  = HMAC-SHA-256(salt = 32×0x00, ikm = c2sKey)
 *   OKM  = HMAC-SHA-256(PRK, info = "ORK-UDP-HS-C2S-v2" || 0x01)   （L = 32，单块）
 *
 * @param c2sKey  32B（Bootstrap V2 Payload 下发的客户端方向会话密钥）
 * @param outKey  32B 输出（调用方栈缓冲；用后即弃，不落日志 / 不序列化）
 */
void DeriveHandshakeKey(std::span<const uint8_t> c2sKey, std::span<uint8_t> outKey);

/**
 * @brief 构造 62B MAC 输入（固定拼接顺序，§5.6 冻结）
 *
 * label(20B) || type(1) || version(1) || connectionId(4 BE)
 * || bootstrapEpoch(16) || clientNonce(16) || counter(4 BE)
 *
 * type / version 写冻结常量值（不从帧读取，防挪用其他 type 的帧重算）。
 */
void BuildHandshakeMacInput(const UdpHandshakeV2Frame& frame,
                            const std::array<uint8_t, kBootstrapEpochSize>& bootstrapEpoch,
                            std::span<uint8_t> outMacInput);

/**
 * @brief 计算 authTag = HMAC-SHA-256(handshakeKey, macInput) 截断前 16B
 */
void ComputeHandshakeAuthTag(std::span<const uint8_t> handshakeKey,
                             std::span<const uint8_t> macInput,
                             std::span<uint8_t> outTag);

/**
 * @brief 逐字段编码 42B 帧（out.size() < 42 或 authTag ≠ 16B 时失败）
 *
 * @param authTag  16B（调用方先经 ComputeHandshakeAuthTag 算好）
 */
bool EncodeHandshakeV2Frame(const UdpHandshakeV2Frame& frame,
                            std::span<const uint8_t> authTag,
                            std::span<uint8_t> out);

/**
 * @brief 一站式构造 42B Handshake V2 包（客户端发送路径）
 *
 * 派生 handshakeKey → 重算 authTag → 逐字段编码；任一步失败返回 false。
 * @param c2sKey         32B
 * @param bootstrapEpoch 16B（本连接 Bootstrap V2 下发的安全域标识）
 * @param out            42B 输出缓冲
 */
bool BuildHandshakeV2Packet(const UdpHandshakeV2Frame& frame,
                            const std::array<uint8_t, kBootstrapEpochSize>& bootstrapEpoch,
                            std::span<const uint8_t> c2sKey,
                            std::span<uint8_t> out);

/**
 * @brief 结构解码 42B 帧（§5.7 步骤 [1]；不做 HMAC）
 *
 * 严格校验帧长 → type → version（首个失败即停），通过后逐字段解析；
 * 任何失败返回 false 且不产生部分结果。
 * authTag 验证由 VerifyHandshakeV2AuthTag 独立承担（查表之后才执行，
 * 未命中 connectionId 的包不做 HMAC — 资源保护）。
 */
bool DecodeHandshakeV2Frame(std::span<const uint8_t> in, UdpHandshakeV2Frame& out,
                            HandshakeV2DecodeError* out_error = nullptr);

/**
 * @brief authTag 常量时间验证（libsodium crypto_verify_16）
 *
 * @param in              完整 42B 帧（authTag 取自帧尾 16B）
 * @param handshakeKey    32B（由连接内 c2sKey 派生）
 * @param bootstrapEpoch  16B（连接内安全域标识；把握手绑定到"这一次 Bootstrap"）
 *
 * 用连接内 epoch 与帧内 nonce/counter 重算期望 tag 后与帧尾常量时间比较；
 * 不读取帧内 type/version 参与 MAC 重算（BuildHandshakeMacInput 写冻结常量）。
 */
bool VerifyHandshakeV2AuthTag(std::span<const uint8_t> in,
                              std::span<const uint8_t> handshakeKey,
                              const std::array<uint8_t, kBootstrapEpochSize>& bootstrapEpoch);

} // namespace ork::codec
