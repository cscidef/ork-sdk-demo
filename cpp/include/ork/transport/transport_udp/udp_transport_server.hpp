#pragma once
#include "ork/connection/logical_connection.hpp"
#include "ork/transport/transport/transport_server.hpp"
#include "ork/transport/transport_udp/udp_callbacks.hpp"
#include "ork/transport/transport_udp/udp_packet_header.hpp"
#include <array>
#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <vector>

/**
 * @brief Phase 4.2: strand 私有 UDP 发送队列条目
 */
struct UdpDatagram
{
    std::vector<uint8_t> data;
    asio::ip::udp::endpoint dest;
};

/**
 * @brief UDP 传输服务器（Phase 4.2: strand 化 + 发送队列）
 *
 * 所有 async 操作绑定到外部传入的 strand executor；
 * 发送统一走 EnqueueSend → strand 私有 deque → 串行 async_send_to。
 */
class UdpTransportServer final : public ITransportServer
{
public:
    using StrandType = asio::strand<asio::io_context::executor_type>;

    UdpTransportServer(asio::io_context& io, Port port, StrandType& strand)
        : socket_(strand),
          port_(port),
          strand_(strand)
    {
    }

    void Start() override;
    void Stop() override;

    /// 兼容旧接口（strand 内调用）：构造 UDP Data 帧并入队发送
    bool SendBytes(Connection& connection, std::span<const uint8_t> payload) override;

    /**
     * @brief 入队发送（strand 内调用，无锁）
     *
     * 复制 datagram 到队列尾部；如果队列之前为空则启动 DoSendNext()。
     * KCP OutputCallback 和 unreliable SendViaUdp 统一走此接口。
     */
    void EnqueueSend(std::vector<uint8_t>&& data, const asio::ip::udp::endpoint& dest);

    /// 排空发送队列（Stop 时调用，strand 内）
    void DrainSendQueue();

    void SetCallbacks(UdpServerCallbacks&& callbacks)
    {
        onHandshakeCallback_ = std::move(callbacks.onHandshake);
        onMessageCallback_ = std::move(callbacks.onMessage);
        onRawPacketCallback_ = std::move(callbacks.onRawPacket);
    }

    /**
     * @brief 设置 UDP socket 内核缓冲区请求值（Phase 4.4，Start() 前调用）
     *
     * 单位字节；<=0 表示沿用 OS 默认（不显式 setsockopt）。
     * 实际生效值可能被 OS clamp，Start() 会读取并写日志。
     */
    void SetSocketBufferSizes(int recvBytes, int sendBytes)
    {
        recvBufBytes_ = recvBytes;
        sendBufBytes_ = sendBytes;
    }

    asio::ip::udp::socket& GetSocket() { return socket_; }

    /**
     * @brief UDP 发送队列瞬时长度（Phase 4.5，strand 内调用）
     */
    std::size_t GetSendQueueSize() const { return sendQueue_.size(); }

    /**
     * @brief UDP 发送队列历史高水位（Phase 4.5，strand 内调用）
     *
     * 在 EnqueueSend 入队后取 max(峰值, 当前长度)；任何一次发送都会使
     * 高水位 >= 1（入队瞬间队列非空，async_send 稍后才排空）。
     */
    std::size_t GetSendQueueHighWater() const { return sendQueueHighWater_; }

    /// UDP 发送队列容量上限；超出时丢弃新数据报并计入 drops_queue_full。
    static constexpr std::size_t kMaxSendQueue = 4096;

private:
    void StartReceive();
    void OnReceive(std::error_code ec, std::size_t bytes);
    void DoSendNext();

    /// Phase 4.4: 应用 socket 缓冲区选项并日志输出生效值（Start() 内调用）
    void ApplySocketBufferOptions();

    void HandlePacket(const asio::ip::udp::endpoint& from, std::span<const uint8_t> packet);
    void HandleData(const asio::ip::udp::endpoint& from, const UdpDataHeader& header, std::span<const uint8_t> payload);

private:
    asio::ip::udp::socket socket_;
    Port port_;
    StrandType& strand_;
    asio::ip::udp::endpoint remoteEndpoint_;

    std::array<uint8_t, 1500> recvBuffer_{};

    /// strand 私有发送队列（无锁，仅在 strand 内访问）
    std::deque<UdpDatagram> sendQueue_;
    bool sending_ = false;

    /// Phase 4.5：发送队列历史高水位（strand 内，EnqueueSend 更新）
    std::size_t sendQueueHighWater_ = 0;

    /// Phase 4.4: UDP socket 缓冲区请求值（字节，<=0 表示 OS 默认）
    int recvBufBytes_ = 0;
    int sendBufBytes_ = 0;

    /// B1：Handshake V1 拒绝采样日志计数器（strand 私有；1/64 采样，禁止每包日志）
    uint64_t v1RejectSampleCounter_ = 0;

    OnUdpHandshakeCallback onHandshakeCallback_;
    OnUdpMessageCallback onMessageCallback_;
    OnUdpRawPacketCallback onRawPacketCallback_;
};
