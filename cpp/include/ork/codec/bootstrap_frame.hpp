#pragma once
/**
 * @file bootstrap_frame.hpp
 * @brief BootstrapFrame — Bootstrap V2（109B）编解码（Rev 3 硬切单轨）
 *
 * 取代 protobuf AssignConnection 消息；V1（81B）自 Rev 3 起被拒绝（§5.9 硬切），
 * 本类型只产生 / 只接受 V2 线格式：
 *
 *   帧 = Envelope(12B) + Payload(97B) = 109B，全网络字节序（大端）
 *   Envelope: magic "ORKB" | version=2 | type=0x01 | reserved=0 | payloadLength=97
 *   Payload : connectionId(4) + sessionToken(4) + bootstrapEpoch(16)
 *             + c2sKey(32) + s2cKey(32) + expireAtMs(8) + flags(1)
 *
 * 线路编码规则（Rev 3 §5.2 强制）：
 *   - 发送方逐字段写入、接收方逐字段读取；
 *   - 禁止把 C++ struct 整块发送 / memcpy —— 不得依赖编译器布局、对齐与填充；
 *   - 全部常量与字段偏移只引用 bootstrap_protocol_constants.hpp（§14.1 唯一权威）。
 *
 * 校验顺序（Rev 3 §5.3 冻结，首个失败即停）：
 *   ① 外层 length prefix = 109（ValidateBootstrapOuterLength，由调用方在帧读取前执行）
 *   ② magic → ③ version → ④ type → ⑤ reserved → ⑥ payloadLength → Payload 字段
 *   任一步失败 → 调用方立即关闭连接：不尝试 V1 解析、不回退明文、不做宽容解析。
 */

#include "ork/codec/bootstrap_protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ork::codec {

/// Bootstrap V2 解码失败原因（按 §5.3 冻结校验顺序；首个失败即停）
enum class BootstrapDecodeError : uint8_t
{
    None = 0,
    /// ① 外层 4B 大端 length prefix ≠ 109（M9 / M10 / M11）
    OuterLengthMismatch,
    /// 帧字节数 ≠ 109：截断（M8）、超长（M9 尾字节形态）、V1 81B 形态（M11）
    BadFrameSize,
    /// ② magic ≠ "ORKB"（M1）
    BadMagic,
    /// ③ version ≠ 2：含 V1 嗅探 0x0001（M2）与未知版本（M3）
    UnsupportedVersion,
    /// ④ type ≠ 0x01（M4）
    BadType,
    /// ⑤ reserved ≠ 0（M5）
    BadReserved,
    /// ⑥ payloadLength ≠ 97（M6 / M7）
    BadPayloadLength,
    /// Payload 语义深检：flags 保留位置位（M12）
    BadFlags,
};

/// 错误可读名（日志 / 测试断言用；不含任何字段值）
const char* ToString(BootstrapDecodeError error) noexcept;

/**
 * @brief 外层 4B 大端 length prefix 校验（§5.3 步骤①）
 *
 * 与帧解码共用冻结语义：prefix 值必须严格等于 109（0x0000006D），
 * 且该值不包含 prefix 自身的 4 字节（§5.2 长度核算）。
 * 调用方（客户端 OnBootstrapLength）与单测引用同一函数，保证生产路径
 * 与校验测试一一对应。
 */
inline constexpr bool ValidateBootstrapOuterLength(uint32_t declaredLength) noexcept
{
    return declaredLength == static_cast<uint32_t>(kBootstrapFrameSize);
}

/**
 * @brief Bootstrap V2 帧（字段值对象；Envelope 字段为冻结常量，不入内存模型）
 */
struct BootstrapFrame
{
    static constexpr size_t kSize = kBootstrapFrameSize;          ///< 109
    static constexpr size_t kEnvelopeSize = kBootstrapEnvelopeSize; ///< 12
    static constexpr size_t kPayloadSize = kBootstrapPayloadSize;   ///< 97

    uint32_t connectionId = 0;
    uint32_t sessionToken = 0;
    /// 本次 Bootstrap 实例的安全域标识（16B CSPRNG）；不承担传输职责前的原始字节
    std::array<uint8_t, kBootstrapEpochSize> bootstrapEpoch{};
    std::array<uint8_t, kSessionKeySize> c2sKey{};
    std::array<uint8_t, kSessionKeySize> s2cKey{};
    uint64_t expireAtMs = 0;
    uint8_t flags = 0; ///< bit 0: encryption_required；其余位发送方必须置 0

    /**
     * @brief 逐字段序列化 109B V2 帧到 out（out.size() < kSize 时失败）
     *
     * Envelope 字段（magic / version / type / reserved / payloadLength）写冻结值。
     */
    bool Encode(std::span<uint8_t> out) const;

    /**
     * @brief 从 in 逐字段解码 V2 帧（要求 in.size() == 109，严格相等）
     *
     * 严格按 §5.3 顺序校验 Envelope（magic → version → type → reserved →
     * payloadLength），全部通过后才解析 Payload；任何失败返回 false 且
     * 不产生部分结果（out 保持原样）。
     *
     * @param out_error 可选输出：首个失败的步骤（OuterLengthMismatch 除外，
     *                  该步由 ValidateBootstrapOuterLength 在帧读取前完成）
     */
    static bool Decode(std::span<const uint8_t> in, BootstrapFrame& out,
                       BootstrapDecodeError* out_error = nullptr);
};

} // namespace ork::codec
