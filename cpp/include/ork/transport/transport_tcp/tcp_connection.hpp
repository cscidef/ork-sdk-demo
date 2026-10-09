#pragma once
#include "asio.hpp"
#include "ork/transport/codec/length_prefixed_message_codec.hpp"
#include "ork/transport/data/network_defs.hpp"
#include "ork/transport/transport_tcp/tcp_callbacks.hpp"
#include "ork/transport/utils/concurrent_queue.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>

#if defined(ORK_HAVE_TLS)
#include <asio/ssl.hpp>
#include <functional>
#endif

/**
 * @brief TCP 连接（明文 / TLS 1.3 双形态，Bootstrap Security A1.1）
 *
 * - 明文形态：现有路径（PlaintextDevelopment，仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON
 *   构建可用，由 TcpTransportServer 决定是否创建）。
 * - TLS 形态（ORK_HAVE_TLS）：构造时把 socket move 进 ssl::stream；accept 后
 *   先 StartTlsHandshake（服务端），握手成功才 Start() 读循环并上报
 *   onConnected（Bootstrap V2 在 TLS 通道内发送）。读写/关闭全部走 ssl stream。
 * - ssl::context 生命周期：TcpConnection 持有 shared_ptr<ssl::context>
 *   （tlsContext_），覆盖所有 stream 使用期。
 * - strand：所有操作经连接私有 strand 串行（与 Phase 4.2 模型一致）。
 */
class TcpConnection
    : public std::enable_shared_from_this<TcpConnection>
{
public:
    explicit TcpConnection(asio::ip::tcp::socket socket, OnTcpRawMessageCallback onMessageCallback, OnTcpDisconnectedCallback onDisconnected)
        : socket_(std::move(socket)),
          strand_(asio::make_strand(socket_.get_executor())),
          codec_(),
          onMessageCallback_(std::move(onMessageCallback)),
          OnDisconnected(std::move(onDisconnected))
    {
        socket_.set_option(asio::ip::tcp::no_delay(true));
    }

#if defined(ORK_HAVE_TLS)
    /**
     * @brief TLS 形态构造（TlsServerAuthenticated 模式）
     *
     * @param tlsContext  ssl::context（服务端证书链/私钥已加载；本连接持有
     *                    shared_ptr，生命周期覆盖 stream）
     * @param socket      已 accept 的 socket（move 进 ssl::stream）
     */
    TcpConnection(std::shared_ptr<asio::ssl::context> tlsContext,
                  asio::ip::tcp::socket socket,
                  OnTcpRawMessageCallback onMessageCallback,
                  OnTcpDisconnectedCallback onDisconnected)
        : tlsContext_(std::move(tlsContext)),
          socket_(std::move(socket)),
          strand_(asio::make_strand(socket_.get_executor())),
          codec_(),
          onMessageCallback_(std::move(onMessageCallback)),
          OnDisconnected(std::move(onDisconnected))
    {
        socket_.set_option(asio::ip::tcp::no_delay(true));
        // socket move 进 ssl::stream；此后本对象所有 IO 均走 tlsStream_
        tlsStream_.emplace(std::move(socket_), *tlsContext_);
    }

    /**
     * @brief A1.1：启动服务端 TLS 握手（accept 后、Start() 之前调用）
     *
     * - 独立超时 timer（timeoutMs），超时 close 底层 socket 使挂起的握手
     *   立即失败（释放槽位；连接纳入 A0.2 pendingTls 预算）。
     * - onDone 在连接 strand 内恰好执行一次：ec 为空即握手成功，调用方
     *   （TcpTransportServer）随后 Start() 并上报 onConnected。
     * - Close() 与握手并发安全（closed_ 后握手以 operation_aborted 完成）。
     */
    void StartTlsHandshake(int timeoutMs, std::function<void(std::error_code)> onDone);
#endif

    void Start();
    void SendBytes(std::span<const uint8_t> data);
    void Close();
    void SetConnectionId(ConnectionId id)
    {
        connId_ = id;
    }

    /// TLS 形态（ssl::stream 持有 socket；IO 全部走 TLS）
    bool IsTls() const
    {
#if defined(ORK_HAVE_TLS)
        return tlsStream_.has_value();
#else
        return false;
#endif
    }

private:
    void DoRead();
    void DoWrite();
    void ShutdownAndNotify();  // strand 内：关闭 socket（TLS 尽力 close_notify）并上报断开
    void NotifyDisconnected(); // strand 内

private:
    // 声明顺序即初始化顺序：TLS 形态下 socket_ 先 move 构造，
    // 再由构造体把 socket_ move 进 tlsStream_（此后 socket_ 处于 moved-from）。
#if defined(ORK_HAVE_TLS)
    std::shared_ptr<asio::ssl::context> tlsContext_;  ///< 生命周期覆盖 stream
    std::optional<asio::ssl::stream<asio::ip::tcp::socket>> tlsStream_;
#endif
    asio::ip::tcp::socket socket_;
    asio::strand<asio::ip::tcp::socket::executor_type> strand_;
    ConnectionId connId_;

    std::array<Byte, 4096> readBuffer_;
    std::deque<std::vector<Byte>> writeQueue_;
    LengthPrefixedMessageCodec codec_;
    OnTcpRawMessageCallback onMessageCallback_;
    bool closed_ = false;

#if defined(ORK_HAVE_TLS)
    /// A1.1：握手超时 timer（strand 私有；仅握手期间存在）
    std::unique_ptr<asio::steady_timer> handshakeTimer_;
    bool handshakeTimedOut_ = false;
#endif

    OnTcpDisconnectedCallback OnDisconnected;
};
