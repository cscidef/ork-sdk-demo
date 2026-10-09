#pragma once
#include <cstdint>
#include <span>

class Connection;

class ITransportServer
{
public:
    virtual ~ITransportServer() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool SendBytes(Connection& connection, std::span<const uint8_t> payload) = 0;
};
