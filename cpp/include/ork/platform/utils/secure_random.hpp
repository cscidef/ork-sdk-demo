#pragma once
/**
 * @file secure_random.hpp
 * @brief 安全敏感随机值的唯一生成入口（libsodium CSPRNG）— Bootstrap Security A0.1
 *
 * 收口规则（doc/BootstrapSecurityDesign.md Rev 3，验收项 #21）：
 *   - 任何安全敏感随机值（SessionToken、未来 Bootstrap V2 的
 *     bootstrapEpoch / clientNonce、key/nonce 材料）必须经本文件生成；
 *     禁止回退到 std::mt19937 / rand / 时间种子 —— libsodium 初始化失败
 *     直接 std::abort()，不做静默降级。
 *   - sessionToken 仅为连接关联字段，不承担安全秘密职责：CSPRNG 来源是
 *     消除可预测性的纵深防御，不是认证强度声明 —— 认证由 OrkCipher AEAD
 *     的 AAD 绑定（28B，含 connectionId+sessionToken）承担。SessionToken
 *     保持 uint32_t，不改 OrkCipher AAD 布局与 kAadSize（Rev 3 冻结）。
 *   - BootstrapEpoch / ClientNonce 为 Bootstrap V2 预留的固定 16B 值类型：
 *     本阶段只提供值语义与 CSPRNG 生成方法，不接入任何 wire protocol，
 *     不引入全局实例等业务状态（A1 阶段接入协议时再定持有关系）。
 */

#include <array>
#include <cstdint>
#include <span>

namespace ork::platform {

/**
 * @brief CSPRNG 安全随机源（libsodium randombytes_buf）
 *
 * 仅静态工具类。首次调用经 std::call_once 执行 sodium_init()；与
 * ork::crypto::OrkCipher 内部各自的初始化互不冲突（sodium_init 幂等
 * 且线程安全，两侧至多各执行一次 call_once）。
 */
class SecureRandom
{
public:
    SecureRandom() = delete;

    /**
     * @brief 以 CSPRNG 填充 out 全部字节（out.size() == 0 时为无操作）
     *
     * 唯一底层原语；key/nonce/epoch 等定长值类型经由它填充。
     */
    static void Fill(std::span<uint8_t> out);

    /**
     * @brief 生成非零 uint32（SessionToken 等连接关联字段的既有约定）
     *
     * 非零约定：0 表示"未赋值/无效"，命中零则重试（单次概率 2^-32，
     * 非实际循环风险）。
     * 唯一性：不查重 —— 连接身份由 connectionId 唯一确定，token 相撞
     * 仅是两连接的关联值相同；RebindTcp / HandleUdpHandshakeV2 均先按
     * connectionId 查表，正确性不受影响。
     */
    static uint32_t U32NonZero();
};

/**
 * @brief Bootstrap V2 预留：固定 16 字节安全随机值类型（bootstrapEpoch）
 *
 * 服务端实例级随机标识（Rev 3 §5.4）。与 ClientNonce 同构但为独立类型，
 * 不可互相赋值/比较，防止跨语义混用。本阶段不接入 wire protocol。
 */
struct BootstrapEpoch
{
    static constexpr size_t kSize = 16;

    std::array<uint8_t, kSize> bytes{};  ///< 值初始化为全零（Zero 语义）

    /// 全零值（"未赋值/无效"哨兵）
    static BootstrapEpoch Zero();

    /// 以 CSPRNG 生成（randombytes_buf，128 位）
    static BootstrapEpoch Generate();

    [[nodiscard]] bool IsZero() const;

    bool operator==(const BootstrapEpoch&) const = default;
    bool operator!=(const BootstrapEpoch&) const = default;
};

/**
 * @brief Bootstrap V2 预留：固定 16 字节安全随机值类型（clientNonce）
 *
 * 每次握手随机生成的客户端 nonce（Rev 3 §5.6）。本阶段不接入 wire protocol。
 */
struct ClientNonce
{
    static constexpr size_t kSize = 16;

    std::array<uint8_t, kSize> bytes{};

    static ClientNonce Zero();

    /// 以 CSPRNG 生成（randombytes_buf，128 位）
    static ClientNonce Generate();

    [[nodiscard]] bool IsZero() const;

    bool operator==(const ClientNonce&) const = default;
    bool operator!=(const ClientNonce&) const = default;
};

} // namespace ork::platform
