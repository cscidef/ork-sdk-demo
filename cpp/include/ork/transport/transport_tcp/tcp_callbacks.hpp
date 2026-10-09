#pragma once
#include "ork/transport/data/network_defs.hpp"
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

class TcpConnection;

using OnTcpConnectedCallback = std::function<void(std::shared_ptr<TcpConnection>)>;
using OnTcpDisconnectedCallback = std::function<void(ConnectionId)>;
/// TCP 入站回调：传递原始字节（length-prefix 解帧后），不再解析 protobuf
using OnTcpRawMessageCallback = std::function<void(ConnectionId, std::vector<Byte>&&)>;

struct TcpServerCallbacks
{
    OnTcpConnectedCallback onConnected;
    OnTcpDisconnectedCallback onDisconnected;
    OnTcpRawMessageCallback onMessage;

#if defined(ORK_HAVE_TLS)
    /**
     * @brief A1.1：TLS 握手结果回调（pump strand 内调用；可为空）
     *
     * ec 为空 = 握手成功（连接已/即将上报 onConnected）；
     * ec == asio::error::timed_out = 握手超时；其他 = 握手失败（静默关闭，
     * 不写日志防 DoS，仅 Metrics 聚合计数）。
     */
    std::function<void(std::error_code)> onTlsHandshakeComplete;
#endif
};
