/**
 * @file sender.hpp
 * @brief Phase 3.3 opaque ISender：Send + Broadcast API
 *
 * ISender 是业务层发送 opaque 字节的公共接口。
 * Sender 是具体实现，依赖 ITransportAdapter 访问底层传输。
 *
 * 设计边界：
 * - payload 为 opaque 字节，Sender 不解释内容
 * - 明文 payload 只拷贝一份；逐连接独立加密由传输层完成
 * - Send/Broadcast 返回后，调用者可释放 payload
 * - TrySend 返回 bool：true=入队成功；false=连接不存在或底层失败
 *   Sender 无法区分 QueueFull / CipherFailed，统一映射为 BadState
 *
 * @see BroadcastReport 逐字段说明
 */

#pragma once

#include "ork/sdk/channel.hpp"
#include "ork/sdk/connection_set.hpp"
#include "ork/sdk/result.hpp"
#include "ork/connection/connection_state.hpp"
#include "ork/platform/types.hpp"
#include "ork/platform/data/kcp_config.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ork::sdk {

// ============================================================================
// BroadcastReport：广播结果累计报告
// ============================================================================

struct BroadcastReport
{
    uint32_t attempted           = 0;  ///< 尝试发送的总连接数
    uint32_t sent                = 0;  ///< 成功发送数
    uint32_t failedNotConnected  = 0;  ///< 连接不存在 / 已断开
    uint32_t failedTooLarge      = 0;  ///< payload 超限（Broadcast 只会在入口处记一次）
    uint32_t failedQueueFull     = 0;  ///< 队列满（当前底层无法区分，见 Sender 注释）
    uint32_t failedCipher        = 0;  ///< 加密失败（当前底层无法区分，见 Sender 注释）

    /// 一致性断言：attempted == 各分类之和
    [[nodiscard]] bool IsConsistent() const noexcept
    {
        return attempted == sent + failedNotConnected + failedTooLarge
                             + failedQueueFull + failedCipher;
    }
};

// ============================================================================
// ITransportAdapter：Sender 对底层传输的抽象依赖
// ============================================================================

/**
 * @brief 传输层适配接口（供 Sender 依赖注入）
 *
 * 生产环境由 NetworkTransportAdapter 包装 NetworkTransport；
 * 测试环境由 FakeTransportAdapter 模拟。
 *
 * TrySend 语义：
 * - true：payload 已入传输层内部缓冲区（调用者随后可释放 span）
 * - false：连接不存在或底层发送失败（Sender 保守映射为 BadState）
 *
 * @note 边界说明：当前 NetworkTransport::TrySend 只返回 bool，
 *       无法区分 "队列满" 与 "加密失败" 与 "无路由"。
 *       因此 Sender 将 false 统一映射为 BadState。
 *       待 Phase 3.4 传输层提供结构化返回值后可细化。
 */
class ITransportAdapter
{
public:
    virtual ~ITransportAdapter() = default;

    /// 向单个连接发送 opaque 字节（同步，返回后调用者可释放 payload）
    virtual bool TrySend(ConnectionId conn, std::span<const uint8_t> payload, Channel channel) = 0;

    /// 遍历所有连接（含 Disconnected），供 Broadcast 使用
    virtual void ForEachConnection(
        const std::function<void(ConnectionId, ConnectionState)>& fn) const = 0;

    /// 最大包大小（字节），Sender 据此做 TooLarge 检查
    virtual uint32_t GetMaxPacketSize() const = 0;
};

// ============================================================================
// ISender：公共发送接口
// ============================================================================

class ISender
{
public:
    virtual ~ISender() = default;

    /**
     * @brief 单连接发送
     * @param conn       目标连接 ID
     * @param payload    opaque 字节（返回后可释放）
     * @param channel    通道选择
     * @return SendResult 结构化结果
     */
    virtual SendResult Send(ConnectionId conn,
                            std::span<const uint8_t> payload,
                            Channel channel) = 0;

    /**
     * @brief 广播发送
     * @param targets    目标连接集合（All / Of / Where）
     * @param payload    opaque 字节（只拷贝一份，逐连接独立加密由传输层完成）
     * @param channel    通道选择
     * @return BroadcastReport 累计报告
     */
    virtual BroadcastReport Broadcast(const ConnectionSet& targets,
                                      std::span<const uint8_t> payload,
                                      Channel channel) = 0;
};

// ============================================================================
// Sender：具体实现
// ============================================================================

/**
 * @brief opaque ISender 实现
 *
 * 构造时注入 ITransportAdapter（生产 = NetworkTransportAdapter）。
 * 所有发送通过 adapter 完成；Sender 只做参数校验与 TooLarge 检查。
 */
class Sender final : public ISender
{
public:
    explicit Sender(ITransportAdapter& adapter)
        : adapter_(adapter)
    {
    }

    SendResult Send(ConnectionId conn,
                    std::span<const uint8_t> payload,
                    Channel channel) override;

    BroadcastReport Broadcast(const ConnectionSet& targets,
                              std::span<const uint8_t> payload,
                              Channel channel) override;

private:
    ITransportAdapter& adapter_;
};

} // namespace ork::sdk

// ============================================================================
// NetworkTransportAdapter：生产环境适配器（声明）
// ============================================================================

class NetworkTransport;  // 全局命名空间前向声明

namespace ork::sdk {

/**
 * @brief 将 NetworkTransport 适配为 ITransportAdapter
 *
 * TrySend 调用 NetworkTransport::TrySend（packetType = FrameType::Data）。
 * ForEachConnection 委托 NetworkTransport::ForEachConnection。
 * GetMaxPacketSize 返回 TransportConfig::maxPacketSize（默认 kDefaultMaxPacketSize(1200)，
 * 生产路径由 NetworkConfig YAML 填充，范围 [50, 1414]）。
 */
class NetworkTransportAdapter final : public ITransportAdapter
{
public:
    explicit NetworkTransportAdapter(::NetworkTransport& transport,
                                     uint32_t maxPacketSize = ork::transport::kDefaultMaxPacketSize);

    bool TrySend(ConnectionId conn,
                 std::span<const uint8_t> payload,
                 Channel channel) override;

    void ForEachConnection(
        const std::function<void(ConnectionId, ConnectionState)>& fn) const override;

    uint32_t GetMaxPacketSize() const override;

private:
    ::NetworkTransport& transport_;
    uint32_t maxPacketSize_;
};

} // namespace ork::sdk
