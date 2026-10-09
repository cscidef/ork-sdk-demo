#pragma once

enum class ConnectionState
{
    TcpOnly,        // TCP established, but UDP handshake not completed
    UdpHandshaking, // Optional, UDP handshake in progress
    Established,    // TCP + UDP both ready
    Disconnected    // TCP disconnected, waiting for reconnection
};
