#pragma once
#include <cstdint>

namespace ork::codec {

/// 帧类型，填入 15B 帧头的 packet_type 字段（offset 11，4 字节 LE）
enum class FrameType : uint32_t {
    Bootstrap    = 0x01,  // 服务端 → 客户端：连接建立，携带 BootstrapFrame 载荷
    BootstrapAck = 0x02,  // 客户端 → 服务端：确认 bootstrap
    Data         = 0x10,  // 双向：opaque 游戏数据
    KeepAlive    = 0x11,  // 双向：心跳
    Disconnect   = 0x20,  // 服务端 → 客户端：主动断开
};

} // namespace ork::codec
