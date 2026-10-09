#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace ork::codec {

// Big-endian (network byte order) encode/decode utilities
// 用于 BootstrapFrame（V2：109B Envelope+Payload）的固定字段编解码

inline void WriteU16BE(std::span<uint8_t> buf, size_t offset, uint16_t value) {
    buf[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

inline uint16_t ReadU16BE(std::span<const uint8_t> buf, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(buf[offset + 0]) << 8) |
                                 static_cast<uint16_t>(buf[offset + 1]));
}

inline void WriteU32BE(std::span<uint8_t> buf, size_t offset, uint32_t value) {
    buf[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

inline uint32_t ReadU32BE(std::span<const uint8_t> buf, size_t offset) {
    return (static_cast<uint32_t>(buf[offset + 0]) << 24) |
           (static_cast<uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<uint32_t>(buf[offset + 2]) << 8) |
           static_cast<uint32_t>(buf[offset + 3]);
}

inline void WriteU64BE(std::span<uint8_t> buf, size_t offset, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        buf[offset + i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

inline uint64_t ReadU64BE(std::span<const uint8_t> buf, size_t offset) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result = (result << 8) | static_cast<uint64_t>(buf[offset + i]);
    }
    return result;
}

} // namespace ork::codec
