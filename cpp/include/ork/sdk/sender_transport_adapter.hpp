/**
 * @file sender_transport_adapter.hpp
 * @brief NetworkTransportAdapter inline 实现
 *
 * 单独头文件以避免 sender.hpp 拖入 NetworkTransport 全量定义。
 * session_framework.hpp 等需要生产适配器的翻译单元包含此文件。
 */

#pragma once

#include "ork/sdk/sender.hpp"
#include "ork/transport/facade/network_transport.hpp"
#include "ork/codec/frame_type.hpp"

namespace ork::sdk {

inline NetworkTransportAdapter::NetworkTransportAdapter(
    ::NetworkTransport& transport, uint32_t maxPacketSize)
    : transport_(transport)
    , maxPacketSize_(maxPacketSize)
{
}

inline bool NetworkTransportAdapter::TrySend(
    ConnectionId conn, std::span<const uint8_t> payload, Channel channel)
{
    return transport_.TrySend(
               conn,
               static_cast<uint32_t>(ork::codec::FrameType::Data),
               payload,
               ToTransportChannel(channel))
           == ork::platform::SendResult::Ok;
}

inline void NetworkTransportAdapter::ForEachConnection(
    const std::function<void(ConnectionId, ConnectionState)>& fn) const
{
    transport_.ForEachSnapshotConnection(
        [&](ConnectionId id, const ConnectionSnapshotEntry& entry)
        {
            fn(id, entry.state);
        });
}

inline uint32_t NetworkTransportAdapter::GetMaxPacketSize() const
{
    return maxPacketSize_;
}

} // namespace ork::sdk
