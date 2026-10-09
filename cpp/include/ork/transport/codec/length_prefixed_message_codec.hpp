#pragma once

#include "ork/transport/data/network_defs.hpp"
#include <functional>
#include <vector>

class LengthPrefixedMessageCodec
{
public:
    explicit LengthPrefixedMessageCodec(std::size_t maxMessageSize = 1024 * 1024)
        : maxMessageSize_(maxMessageSize)
    {
        buffer_.reserve(4096);
    }

    void OnBytes(const Byte* data, std::size_t len);

    void PrependLength(std::vector<Byte>& message);

private:
    enum class State
    {
        ReadLength,
        ReadPayload
    };

    void TryConsume();

public:
    std::function<void(std::vector<Byte>&&)> OnMessage;

private:
    State state_ = State::ReadLength;

    std::vector<Byte> buffer_;
    std::uint32_t expectedLength_ = 0;

    const std::size_t maxMessageSize_;
};
