#pragma once
#include "ork/transport/data/network_defs.hpp"
#include "ork/platform/data/disconnect_reason.hpp"
#include "ork/platform/data/transport_channel.hpp"
#include <cstdint>
#include <optional>
#include <vector>

/// 包类型（替代原 protobuf MessageType 枚举）
enum class PacketType : uint32_t
{
    System  = 0,
    Command = 1,
};

enum class NetworkMessageType
{
    Connected,
    Disconnected,
};

struct InboundMessage
{
    ConnectionId connId;
    TransportChannel channel;
    PacketType packetType;
    std::vector<uint8_t> payload;              ///< opaque 字节载荷
    std::optional<NetworkMessageType> networkMessage;
    /// packetType==System 且 networkMessage==Disconnected 时的断开原因
    DisconnectReason reason = DisconnectReason::PeerClosed;
};

struct OutboundMessage
{
    ConnectionId connId;
    TransportChannel channel;
    uint32_t packetType;
    std::vector<uint8_t> payload;              ///< opaque 字节载荷
};
