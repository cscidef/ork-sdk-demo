/**
 * @file client_message_dispatcher.hpp
 * @brief Phase 3.4：opaque 字节统一分发到 OnPayload
 */

#pragma once

#include "ork/transport/data/network_defs.hpp"
#include "ork/platform/data/transport_channel.hpp"
#include <cstdint>
#include <vector>

class SessionFrameworkImpl;

class ClientMessageDispatcher
{
public:
    explicit ClientMessageDispatcher(SessionFrameworkImpl& framework)
        : framework_(framework)
    {
    }

    /// 将 opaque 字节载荷统一转发到 framework（不再按 channel 拆分语义）
    void Dispatch(ConnectionId connectionId, TransportChannel channel, const std::vector<uint8_t>& payload);

private:
    SessionFrameworkImpl& framework_;
};
