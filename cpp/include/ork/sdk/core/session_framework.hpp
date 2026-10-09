/**
 * @file session_framework.hpp
 * @brief SessionFramework 实现（SDK 核心引擎，纯传输职责）
 *
 * SessionFrameworkImpl 同时实现：
 * - SessionFramework：pump 循环、监听回调、opaque 分发
 * - IConnectionRegistry：连接查询 / 断开
 *
 * 连接状态单表模型（Phase 3.4）：
 * - transport_ → ConnectionManager（传输层，唯一连接状态源）
 * - 玩家/Join/房间状态已下沉 examples，core 不再持有 clients_ 表
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "ork/connection/connection_state.hpp"
#include "ork/connection/logical_connection.hpp"
#include "ork/platform/context/session_context.hpp"
#include "ork/transport/facade/network_transport.hpp"
#include "ork/sdk/message_adapter/client_message_dispatcher.hpp"
#include "ork/sdk/sender_transport_adapter.hpp"
#include "ork/sdk/sdk/session_interface.hpp"

class SessionFrameworkImpl final : public SessionFramework, public IConnectionRegistry
{
public:
    explicit SessionFrameworkImpl(const TransportConfig& config, NetworkTransport& transport, RuntimeContext& context)
        : config_(config),
          transport_(transport),
          senderAdapter_(transport, static_cast<uint32_t>(config.maxPacketSize)),
          sender_(senderAdapter_),
          dispatcher_(*this),
          sessionContext_(context)
    {
        transport_.SetKcpEnabled(config.enableKcp);
    }

    void RegisterListener(ork::sdk::IHostListener* listener) override
    {
        listener_ = listener;
    }

    void Start() override
    {
        running_ = true;
    }

    void Stop() override
    {
        running_ = false;
    }

    ork::sdk::ISender& GetSender() override { return sender_; }

    IConnectionRegistry& GetConnectionRegistry() override { return *this; }

    const TransportConfig& GetConfig() const override { return config_; }

    // ---- IConnectionRegistry（见 session_interface.hpp） ----

    size_t Count() const override;
    bool TryGet(ConnectionId connectionId, ConnectionView& out) const override;
    void ForEach(const std::function<void(const ConnectionView&)>& fn) const override;
    std::vector<ConnectionView> Snapshot() const override;
    bool Disconnect(ConnectionId connectionId, DisconnectReason reason) override;

    // ---- 网络/消息回调（ClientMessageDispatcher 与 pump 循环驱动） ----
    void HandlePayload(ConnectionId connectionId, TransportChannel channel,
                       const std::vector<uint8_t>& payload);

    /**
     * @brief 宿主 pump 循环体（Phase 4.3：由 NetworkTransport 在 strand 内以 pumpRate 频率驱动）
     *
     * KCP Update / Recv 以 kcpUpdateRate 在 strand timer 每拍执行；本方法由
     * NetworkTransport::HostPump 经累加器分频后回调，只做：
     * 入站消息分发（回调 IHostListener）+ 周期性 OnStats 上报。
     *
     * @param inbound 本轮积累的入站消息批次（由 transport 持有，回调返回后丢弃）
     */
    void HostPump(std::vector<InboundMessage>& inbound);

private:
    /** @brief 组装 ConnectionView（仅传输字段，来自只读快照条目） */
    ConnectionView BuildConnectionView(ConnectionId connectionId, const ConnectionSnapshotEntry& entry) const;

    /** @brief 采集 OrkStats 并回调 listener_（steady_clock deadline，约每秒一次） */
    void MaybeReportStats();

    ork::sdk::OrkStats CollectStats() const;

private:
    TransportConfig config_;
    std::atomic<bool> running_{false};
    ork::sdk::IHostListener* listener_ = nullptr;
    NetworkTransport& transport_;
    ork::sdk::NetworkTransportAdapter senderAdapter_;
    ork::sdk::Sender sender_;
    ClientMessageDispatcher dispatcher_;

    /// 下次 OnStats 上报的绝对截止点（steady_clock）。Phase 4.3 Step 5：改为时间
    /// deadline 驱动（每 ~1s 一次），不再按 pumpTick 计数，避免上报周期随泵频漂移。
    std::chrono::steady_clock::time_point nextStatsReport_{};
    SessionContext sessionContext_;
};
