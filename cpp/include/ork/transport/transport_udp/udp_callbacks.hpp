#pragma once

#include "ork/transport/data/inout_bound_message.hpp"
#include "ork/transport/data/network_defs.hpp"
#include <asio.hpp>
#include <span>

using UdpEndpoint = asio::ip::udp::endpoint;

/**
 * @brief B1：Handshake V2 回调 —— 传递完整 42B 原始帧（含 authTag）
 *
 * 结构解码 / connectionId 查表 / 有效期 / authTag 常量时间验证 / 防重放
 * 均由上层（NetworkTransport，strand 内）按 Rev 3 §5.7 顺序执行；
 * 本层只做首字节路由（type == HandshakeV2 且 exact 42B）。
 */
using OnUdpHandshakeCallback = std::function<void(const UdpEndpoint&, std::span<const uint8_t> packet)>;
using OnUdpMessageCallback = std::function<void(const UdpEndpoint&, ConnectionId, std::span<const uint8_t>)>;
using OnUdpRawPacketCallback = std::function<void(const UdpEndpoint&, std::span<const uint8_t>)>;

struct UdpServerCallbacks
{
    OnUdpHandshakeCallback onHandshake;
    OnUdpMessageCallback onMessage;
    OnUdpRawPacketCallback onRawPacket;
};
