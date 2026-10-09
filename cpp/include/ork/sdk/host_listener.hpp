/**
 * @file host_listener.hpp
 * @brief Phase 3.4 宿主监听接口：core 对外的唯一事件回调面
 *
 * 职责边界：core 只保留连接、传输、opaque payload、网络统计四类事件。
 * 玩家/房间/Join/游戏 Tick 等业务语义一律由宿主（examples）在 OnPayload 之上自行实现。
 *
 * ============================ 线程契约 ============================
 * - 所有回调均在 core 内部 NetworkPump 的 asio strand 线程内触发（Phase 4.3：
 *   由 strand timer 经累加器分频，以 pumpRate 频率驱动宿主 pump 循环）。
 * - 实现必须非阻塞：禁止在回调内执行耗时计算、磁盘 IO、同步锁等待。
 * - 禁止在回调中同步调用 SessionFramework::Stop() / 析构框架：它们会等待
 *   strand / IO 线程退出，而回调正在该线程上执行 → 死锁。需要关停时，
 *   先投递到宿主自己的线程再异步执行。
 * - IConnectionRegistry::Disconnect() 可在回调中调用（内部为异步 socket
 *   close，不阻塞）；触发的 OnDisconnected 会在后续 pump 迭代分发，
 *   不会同步重入当前回调。
 * - OnPayload 的 payload 生命周期仅限本次回调内有效（引用 core 内部缓冲）。
 * ==================================================================
 */

#pragma once

#include "ork/sdk/channel.hpp"
#include "ork/platform/data/disconnect_reason.hpp"
#include "ork/platform/types.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ork::sdk {

/**
 * @brief 网络统计快照（OnStats 携带）
 *
 * Phase 4.5：由 core 内部 Metrics（strand 单写者）采集为值快照，在
 * NetworkPump strand 内的 HostPump（~1Hz）回调宿主。
 *
 * 计数语义：
 * - bytes / packets 系列为 **wire 级**累计（进程启动起，单调递增）；
 * - kcp* 为跨当前存活会话的聚合瞬时值（每次采样重算，会随连接断开回落）；
 * - *QueueLen 为采样瞬时长度，*QueueHighWater 为进程内历史峰值；
 * - activeConnections 为采样瞬间值；
 * - disconnectsByReason 按 DisconnectReason 枚举索引累计。
 *
 * 各字段独立采样，非事务一致。
 */
struct OrkStats
{
    // ---- 连接 ----
    uint64_t activeConnections = 0;  ///< 传输层活跃连接数（linkState != Disconnected）

    // ---- 全局累计（wire 级） ----
    uint64_t bytesIn = 0;            ///< 累计入站字节
    uint64_t bytesOut = 0;           ///< 累计出站字节
    uint64_t packetsIn = 0;          ///< 累计入站包数
    uint64_t packetsOut = 0;         ///< 累计出站包数
    uint64_t dropsTooLarge = 0;      ///< Sender 超限丢弃
    uint64_t dropsQueueFull = 0;     ///< UDP 发送队列满丢弃

    // ---- 队列水位 ----
    uint64_t udpSendQueueLen = 0;        ///< UDP 发送队列瞬时长度
    uint64_t udpSendQueueHighWater = 0;  ///< UDP 发送队列历史高水位
    uint64_t commandQueueLen = 0;        ///< 入站批次瞬时长度（待分发）
    uint64_t inboundQueueHighWater = 0;  ///< 入站批次历史高水位

    // ---- KCP 聚合 ----
    uint64_t kcpWaitsndSum = 0;        ///< 各存活会话 waitsnd 之和
    uint64_t kcpSrttAvgMs = 0;         ///< 各存活会话平滑 RTT 均值（ms）
    uint64_t kcpRetransmitsTotal = 0;  ///< 各存活会话 RTO 重传之和

    // ---- 断开统计（按 DisconnectReason 索引） ----
    std::array<uint64_t, kDisconnectReasonCount> disconnectsByReason{};

    // ---- pump ----
    uint64_t pumpTickCount = 0;      ///< pump 循环迭代数
    uint64_t pumpOverrunCount = 0;   ///< pump 帧超限次数（负载过高信号）

    // ---- 加密失败 ----
    uint64_t cryptoFailAead = 0;      ///< AEAD 验签失败
    uint64_t cryptoFailReplay = 0;    ///< 重放拒绝
    uint64_t cryptoFailExpired = 0;   ///< 密钥过期
    uint64_t cryptoFailMalformed = 0; ///< 帧格式非法

    // ---- 解码失败 ----
    uint64_t decodeFailures = 0;      ///< 非加密的帧/缓冲解码错误

    // ---- 向后兼容别名（旧字段，与新字段等价） ----
    uint64_t recvPackets = 0;        ///< == packetsIn
    uint64_t sendPackets = 0;        ///< == packetsOut
    uint64_t pumpLagCount = 0;       ///< == pumpOverrunCount
};

/**
 * @brief 宿主监听接口（业务方实现，注册到 SessionFramework）
 *
 * 四个回调覆盖 core 的全部对外事件：
 * - OnConnected / OnDisconnected：连接生命周期
 * - OnPayload：opaque 入站载荷（含通道语义，见 ork::sdk::Channel）
 * - OnStats：周期性网络统计（默认约每秒一次）
 */
class IHostListener
{
public:
    virtual ~IHostListener() = default;

    /// 连接就绪（TCP bootstrap + UDP 握手完成）
    virtual void OnConnected(ConnectionId connectionId) = 0;

    /// 连接断开。reason 见 DisconnectReason（纯传输语义）
    virtual void OnDisconnected(ConnectionId connectionId, DisconnectReason reason) = 0;

    /// 收到 opaque 载荷。core 不解释内容；宿主自行定义帧格式与业务语义
    virtual void OnPayload(ConnectionId connectionId,
                            Channel channel,
                            std::span<const uint8_t> payload) = 0;

    /// 周期性网络统计上报
    virtual void OnStats(const OrkStats& stats) = 0;
};

} // namespace ork::sdk
