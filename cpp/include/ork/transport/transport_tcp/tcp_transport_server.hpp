#pragma once

#include "asio.hpp"
#include "ork/connection/logical_connection.hpp"
#include "ork/transport/data/inout_bound_message.hpp"
#include "ork/transport/data/network_defs.hpp"
#include "ork/transport/transport/transport_server.hpp"
#include "ork/transport/transport_tcp/tcp_callbacks.hpp"
#include "ork/transport/transport_tcp/tcp_connection.hpp"
#include "ork/transport/utils/concurrent_queue.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>

#if defined(ORK_HAVE_TLS)
#include <asio/ssl.hpp>
#include <memory>
#endif

class TcpTransportServer final : public ITransportServer
{
public:
    /**
     * @brief 构造 TCP 服务器
     *
     * A0.2：accept 完成回调绑定到 strand（与 NetworkPump / ConnectionManager
     * 同一 strand），使接纳门禁与连接表状态严格串行、无数据竞争。
     *
     * @param io      asio io_context（由外部持有）
     * @param port    监听端口
     * @param strand  accept 回调执行的 strand（NetworkPump strand）
     * @param backlog listen backlog
     */
    TcpTransportServer(asio::io_context& io, Port port,
                       asio::strand<asio::io_context::executor_type>& strand,
                       std::size_t backlog = 128)
        : port_(port),
          backlog_(backlog),
          acceptor_(io),
          strand_(strand)
    {
    }

    ~TcpTransportServer() override
    {
        Stop();
    }

    void Start() override;
    void Stop() override;

    bool SendBytes(Connection& connection, std::span<const uint8_t> payload) override;

    void SetCallbacks(TcpServerCallbacks&& callbacks)
    {
        onConnected_ = std::move(callbacks.onConnected);
        onDisconnected_ = std::move(callbacks.onDisconnected);
        onMessage_ = std::move(callbacks.onMessage);
#if defined(ORK_HAVE_TLS)
        onTlsHandshakeComplete_ = std::move(callbacks.onTlsHandshakeComplete);
#endif
    }

    /**
     * @brief A0.2：设置连接接纳门禁（Start 前调用）
     *
     * accept 完成回调内、创建 TcpConnection（或 TLS 握手）之前调用；
     * 返回 false 则静默关闭 socket（不分配 connectionId/sessionToken、
     * 不回包、不写日志）。回调在 strand 内执行，可安全读取
     * ConnectionManager 的原子计数与限流器状态。
     *
     * A1.1：签名增加 pendingTls（正在 TLS 握手、尚未上报的连接数），
     * 供调用方把"未完成 TLS"纳入 A0.2 未完成 Bootstrap 预算（两层门禁）。
     */
    void SetAdmissionCallback(
        std::function<bool(const asio::ip::tcp::endpoint&, std::size_t pendingTls)> admission)
    {
        admission_ = std::move(admission);
    }

#if defined(ORK_HAVE_TLS)
    /**
     * @brief A1.1：设置 Bootstrap 通道安全（Start 前调用）
     *
     * @param tlsContext               非空 = TlsServerAuthenticated（TLS accept
     *                                 路径：先握手、成功才上报 onConnected）；
     *                                 空 = 明文路径（仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP
     *                                 编入且强制 loopback bind，否则 Start fail closed）
     * @param tlsHandshakeTimeoutMs    TLS 握手超时（默认 5000；独立 timer，
     *                                 超时 close socket 释放槽位）
     */
    void SetBootstrapSecurity(std::shared_ptr<asio::ssl::context> tlsContext,
                              int tlsHandshakeTimeoutMs)
    {
        tlsContext_ = std::move(tlsContext);
        tlsHandshakeTimeoutMs_ = tlsHandshakeTimeoutMs;
    }
#endif

    /**
     * @brief A1.1：正在 TLS 握手、尚未上报的连接数（仅 pump strand 内调用）
     *
     * HostPump 采集 gauge（tls_pending）与 admission 门禁共用。
     */
    std::size_t PendingTlsCount() const
    {
#if defined(ORK_HAVE_TLS)
        return pendingTls_;
#else
        return 0;
#endif
    }

    /** @brief 监听端点（Start 后有效；诊断/测试用，确认 loopback bind） */
    asio::ip::tcp::endpoint LocalEndpoint() const
    {
        std::error_code ec;
        return acceptor_.local_endpoint(ec);
    }

private:
    void DoAccept();
    void OnConnectionRawMessage(ConnectionId id, std::vector<Byte>&& msg);
    void OnDisconnected(ConnectionId id);

#if defined(ORK_ALLOW_PLAINTEXT_BOOTSTRAP)
    /** @brief 明文 accept 路径（仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP 编入；loopback bind） */
    void StartPlainAccept(asio::ip::tcp::socket socket);
#endif
#if defined(ORK_HAVE_TLS)
    /** @brief TLS accept 路径：先握手（独立超时），成功才 Start + onConnected */
    void StartTlsAccept(asio::ip::tcp::socket socket);
#endif

private:
    Port port_;
    std::size_t backlog_;

    asio::ip::tcp::acceptor acceptor_;
    asio::strand<asio::io_context::executor_type>& strand_;
    bool running_ = false;

    OnTcpConnectedCallback onConnected_;
    OnTcpDisconnectedCallback onDisconnected_;
    OnTcpRawMessageCallback onMessage_;

#if defined(ORK_HAVE_TLS)
    /// A1.1：TLS 模式（非空 = TlsServerAuthenticated）；生命周期覆盖所有 stream
    ///（各 TcpConnection 另持 shared_ptr 副本）
    std::shared_ptr<asio::ssl::context> tlsContext_;
    int tlsHandshakeTimeoutMs_ = 5000;
    /// 正在 TLS 握手、尚未上报的连接数（strand 私有）
    std::size_t pendingTls_ = 0;
    /// TLS 握手结果回调（strand 内；Metrics 计数用，可为空）
    std::function<void(std::error_code)> onTlsHandshakeComplete_;
#endif

    /// A0.2：接纳门禁（strand 内调用；拒绝 = 静默关闭 socket）
    std::function<bool(const asio::ip::tcp::endpoint&, std::size_t pendingTls)> admission_;
};
