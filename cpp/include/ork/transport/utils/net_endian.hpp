#pragma once
#include <bit>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static inline bool IsLittleEndian()
{
    return std::endian::native == std::endian::little;
}

template <typename T>
T to_network_order(T value)
{
    if constexpr (sizeof(T) == sizeof(uint16_t))
    {
        uint16_t temp = htons(*reinterpret_cast<uint16_t*>(&value));
        return *reinterpret_cast<T*>(&temp);
    }
    else if constexpr (sizeof(T) == sizeof(uint32_t))
    {
        uint32_t temp = htonl(*reinterpret_cast<uint32_t*>(&value));
        return *reinterpret_cast<T*>(&temp);
    }
    else if constexpr (sizeof(T) == sizeof(uint64_t))
    {
        if (!IsLittleEndian())
        {
            return value;
        }

        const uint32_t high = static_cast<uint32_t>(value >> 32);
        const uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFF);

        return (static_cast<uint64_t>(htonl(low)) << 32) | static_cast<uint64_t>(htonl(high));
    }
    else
    {
        static_assert(sizeof(T) == 0, "Unsupported type for to_network_order");
    }
}

template <typename T>
T from_network_order(T value)
{
    if constexpr ((sizeof(T) == sizeof(uint16_t)))
    {
        uint16_t temp = ntohs(*reinterpret_cast<uint16_t*>(&value));
        return *reinterpret_cast<T*>(&temp);
    }
    else if constexpr (sizeof(T) == sizeof(uint32_t))
    {
        uint32_t temp = ntohl(*reinterpret_cast<uint32_t*>(&value));
        return *reinterpret_cast<T*>(&temp);
    }
    else if constexpr (sizeof(T) == sizeof(uint64_t))
    {
        if (!IsLittleEndian())
        {
            return value;
        }

        const uint32_t high = static_cast<uint32_t>(value >> 32);
        const uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFF);

        return (static_cast<uint64_t>(ntohl(low)) << 32) | static_cast<uint64_t>(ntohl(high));
    }
    else
    {
        static_assert(sizeof(T) == 0, "Unsupported type for to_network_order");
    }
}
