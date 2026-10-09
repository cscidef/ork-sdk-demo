#pragma once
/**
 * @file bootstrap_protocol_constants.hpp
 * @brief Bootstrap V2 / UDP Handshake V2 线格式唯一权威常量（Rev 3 §14.1）
 *
 * 冻结来源：doc/BootstrapSecurityDesign.md Rev 3 第五章（§5.2 / §5.5 / §5.6）
 * 与第十四章 §14.1。本文件是 magic / 版本 / type / reserved / 各字段宽度与
 * 偏移 / 固定总长的唯一定义处：
 *   - C++ 实现、单元测试、demo / Python 侧镜像一律引用本头（或其显式镜像），
 *     **禁止在其它位置复制这些字面量**；
 *   - 校验顺序冻结见 §5.3（先 Envelope 后 Payload），本头只承载常量，
 *     不承载解析流程。
 *
 * 线格式（Rev 3 §5.2 冻结，总长 109B = Envelope 12B + Payload 97B）：
 *
 *   Envelope（12B）
 *     +---------+-------------+----------------------+----------+---------------+
 *     | magic   | version     | type                 | reserved | payloadLength |
 *     | 4B "ORKB" | 2B BE = 2 | 1B = 0x01 (Response) | 1B = 0   | 4B BE = 97    |
 *     +---------+-------------+----------------------+----------+---------------+
 *
 *   Payload（97B）
 *     +--------------+--------------+----------------+--------+--------+------------+-------+
 *     | connectionId | sessionToken | bootstrapEpoch | c2sKey | s2cKey | expireAtMs | flags |
 *     | 4B BE        | 4B BE        | 16B            | 32B    | 32B    | 8B BE      | 1B    |
 *     +--------------+--------------+----------------+--------+--------+------------+-------+
 *
 * 外层 TCP framing：4B 大端 length prefix = 109（0x0000006D），不属于 109B 帧
 * 本体（Rev 3 §5.2 长度核算）。
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ork::codec {

// =============================================================================
// Bootstrap V2 Envelope（12B）— Rev 3 §5.2
// =============================================================================

/// Envelope magic：ASCII "ORKB"（4F 52 4B 42），帧身份标识
inline constexpr std::string_view kBootstrapMagic = "ORKB";
inline constexpr std::size_t kBootstrapMagicSize = 4;

/// 协议版本（2B 大端）。版本演进只能递增此字段（Rev 3 §5.2）
inline constexpr uint16_t kBootstrapVersion = 2;
inline constexpr std::size_t kBootstrapVersionSize = 2;

/// type 字段合法值（1B）：BootstrapMessageType::BootstrapResponse
inline constexpr uint8_t kBootstrapMessageTypeBootstrapResponse = 0x01;
inline constexpr std::size_t kBootstrapMessageTypeSize = 1;

/// reserved 冻结值（1B）：必须为 0，非 0 即拒绝（为未来协商保留）
inline constexpr uint8_t kBootstrapReservedValue = 0x00;
inline constexpr std::size_t kBootstrapReservedSize = 1;

/// payloadLength 字段宽度（4B 大端），值必须等于该 type 的固定长度
inline constexpr std::size_t kBootstrapPayloadLengthSize = 4;

/// Envelope 固定长度：4 + 2 + 1 + 1 + 4 = 12
inline constexpr std::size_t kBootstrapEnvelopeSize =
    kBootstrapMagicSize + kBootstrapVersionSize + kBootstrapMessageTypeSize +
    kBootstrapReservedSize + kBootstrapPayloadLengthSize;

// =============================================================================
// Bootstrap V2 Payload（97B）— Rev 3 §5.2 / §5.4
// =============================================================================

/// connectionId 宽度（4B 大端；公开可预测路由标识，非秘密）
inline constexpr std::size_t kBootstrapConnectionIdSize = 4;

/// sessionToken 宽度（4B 大端；连接关联字段，保持 uint32_t — Rev 3 §5.4）
inline constexpr std::size_t kSessionTokenSize = 4;

/// bootstrapEpoch 宽度（16B；每次 Bootstrap 实例独立生成的 CSPRNG 安全域标识）
inline constexpr std::size_t kBootstrapEpochSize = 16;

/// c2sKey / s2cKey 单方向密钥宽度（32B）
inline constexpr std::size_t kSessionKeySize = 32;

/// expireAtMs 宽度（8B 大端；Unix 毫秒到期时刻）
inline constexpr std::size_t kExpireAtMsSize = 8;

/// flags 宽度（1B）；bit 0 = encryption_required，其余位必须为 0
inline constexpr std::size_t kBootstrapFlagsSize = 1;

/// flags bit 0：encryption_required
inline constexpr uint8_t kBootstrapFlagEncryptionRequired = 0x01;

/// flags 合法位掩码：非法置位即拒绝（Rev 3 §5.2 / 向量组 C M12）
inline constexpr uint8_t kBootstrapFlagsValidMask = kBootstrapFlagEncryptionRequired;

/// Payload 固定长度：4 + 4 + 16 + 32 + 32 + 8 + 1 = 97
inline constexpr std::size_t kBootstrapPayloadSize =
    kBootstrapConnectionIdSize + kSessionTokenSize + kBootstrapEpochSize +
    2 * kSessionKeySize + kExpireAtMsSize + kBootstrapFlagsSize;

/// Envelope.payloadLength 的冻结值（= Payload 固定长度 97 = 0x00000061）
inline constexpr uint32_t kBootstrapPayloadLengthValue =
    static_cast<uint32_t>(kBootstrapPayloadSize);

/// Bootstrap V2 帧总长：12 + 97 = 109（= 0x6D）
inline constexpr std::size_t kBootstrapFrameSize =
    kBootstrapEnvelopeSize + kBootstrapPayloadSize;

/// 外层 TCP length prefix 宽度（4B 大端；prefix 值 = 109，不含 prefix 自身）
inline constexpr std::size_t kBootstrapLengthPrefixSize = 4;

// =============================================================================
// 字段偏移（帧内绝对偏移）— 校验与逐字段编解码的唯一出处
// =============================================================================

// Envelope 字段
inline constexpr std::size_t kBootstrapMagicOffset = 0;
inline constexpr std::size_t kBootstrapVersionOffset =
    kBootstrapMagicOffset + kBootstrapMagicSize;                       // 4
inline constexpr std::size_t kBootstrapTypeOffset =
    kBootstrapVersionOffset + kBootstrapVersionSize;                   // 6
inline constexpr std::size_t kBootstrapReservedOffset =
    kBootstrapTypeOffset + kBootstrapMessageTypeSize;                  // 7
inline constexpr std::size_t kBootstrapPayloadLengthOffset =
    kBootstrapReservedOffset + kBootstrapReservedSize;                 // 8

// Payload 字段（紧接 Envelope 之后）
inline constexpr std::size_t kBootstrapConnectionIdOffset =
    kBootstrapPayloadLengthOffset + kBootstrapPayloadLengthSize;       // 12
inline constexpr std::size_t kBootstrapSessionTokenOffset =
    kBootstrapConnectionIdOffset + kBootstrapConnectionIdSize;         // 16
inline constexpr std::size_t kBootstrapEpochOffset =
    kBootstrapSessionTokenOffset + kSessionTokenSize;                  // 20
inline constexpr std::size_t kBootstrapC2sKeyOffset =
    kBootstrapEpochOffset + kBootstrapEpochSize;                       // 36
inline constexpr std::size_t kBootstrapS2cKeyOffset =
    kBootstrapC2sKeyOffset + kSessionKeySize;                          // 68
inline constexpr std::size_t kBootstrapExpireAtMsOffset =
    kBootstrapS2cKeyOffset + kSessionKeySize;                          // 100
inline constexpr std::size_t kBootstrapFlagsOffset =
    kBootstrapExpireAtMsOffset + kExpireAtMsSize;                      // 108

// =============================================================================
// UDP Handshake V2（42B，type/version 分离）— Rev 3 §5.5（B1 阶段实现）
// =============================================================================

/// UdpPacketType::HandshakeV2 冻结值（首字节为路由 type，与 Handshake=0 / Data=2 并列）
inline constexpr uint8_t kUdpHandshakeV2PacketType = 0x03;

/// Handshake V2 version 字段（第二字节；后续演进递增此字段，type 不变）
inline constexpr uint8_t kUdpHandshakeV2Version = 0x02;

/// clientNonce 宽度（16B，每次握手随机生成，128 位 CSPRNG）
inline constexpr std::size_t kClientNonceSize = 16;

/// counter 宽度（4B 大端；同一 nonce 下的重试单调计数）
inline constexpr std::size_t kHandshakeCounterSize = 4;

/// authTag 截断长度（16B；HMAC-SHA-256 截断取前 16 字节）
inline constexpr std::size_t kAuthTagSize = 16;

/// Handshake V2 帧总长：1 + 1 + 4 + 16 + 4 + 16 = 42
inline constexpr std::size_t kUdpHandshakeV2Size =
    1 + 1 + 4 + kClientNonceSize + kHandshakeCounterSize + kAuthTagSize;

/// MAC 输入域分隔标签（20B ASCII，置于 MAC 输入首部）
inline constexpr std::string_view kHandshakeMacLabel = "ORK-UDP-HANDSHAKE-V2";
inline constexpr std::size_t kHandshakeMacLabelSize = 20;

/// HKDF info label（17B ASCII，"ORK-UDP-HS-C2S-v2"）
inline constexpr std::string_view kHandshakeKeyInfoLabel = "ORK-UDP-HS-C2S-v2";
inline constexpr std::size_t kHandshakeKeyInfoLabelSize = 17;

/// handshakeKey 派生长度（32B）
inline constexpr std::size_t kHandshakeKeySize = 32;

/// MAC 输入总长：20 + 1 + 1 + 4 + 16 + 16 + 4 = 62
inline constexpr std::size_t kHandshakeMacInputSize =
    kHandshakeMacLabelSize + 1 + 1 + kBootstrapConnectionIdSize +
    kBootstrapEpochSize + kClientNonceSize + kHandshakeCounterSize;

} // namespace ork::codec
