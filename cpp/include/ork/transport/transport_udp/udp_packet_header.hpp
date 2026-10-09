#pragma once
#include <cstdint>
#include "ork/codec/bootstrap_protocol_constants.hpp"

enum class UdpPacketType : uint8_t
{
    Handshake = 0,      // V1 9B（B1 起生产路径静默拒绝，仅保留枚举供拒绝计数/测试构造）
    Data = 2,
    HandshakeV2 = 3,    // B1：42B Handshake V2 首字节路由 type（Rev 3 §5.5 冻结值）
};

// take care of byte alignment. type must be at offset 0.

#pragma pack(push, 1)
struct UdpHandshakeHeader
{
    uint8_t type;          // Handshake（V1 线格式事实记录；生产不再编解码）
    uint32_t connectionId; // network order
    uint32_t sessionToken; // network order
};
#pragma pack(pop)

#pragma pack(push, 1)
struct UdpDataHeader
{
    uint8_t type;          // UdpPacketType
    uint32_t connectionId; // network byte order
    uint16_t payloadSize;  // network byte order
};
#pragma pack(pop)

static_assert(sizeof(UdpHandshakeHeader) == 9, "UdpHandshakeHeader size is incorrect");
static_assert(sizeof(UdpDataHeader) == 7, "UdpDataHeader size is incorrect");

// B1：路由 type 枚举值必须与唯一权威常量头一致（§14.1 kUdpHandshakeV2PacketType）
static_assert(static_cast<uint8_t>(UdpPacketType::HandshakeV2) ==
              ork::codec::kUdpHandshakeV2PacketType);
// V2 与 V1/Data 枚举值不冲突（Rev 3 §5.5：type=0x03 避让 Handshake=0 / Data=2）
static_assert(static_cast<uint8_t>(UdpPacketType::HandshakeV2) !=
              static_cast<uint8_t>(UdpPacketType::Handshake));
static_assert(static_cast<uint8_t>(UdpPacketType::HandshakeV2) !=
              static_cast<uint8_t>(UdpPacketType::Data));
