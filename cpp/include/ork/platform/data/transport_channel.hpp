#pragma once
#include <cstdint>

enum class TransportChannel : uint8_t
{
    Reliable   = 0,
    Unreliable = 1
};
