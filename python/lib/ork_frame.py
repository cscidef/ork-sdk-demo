"""
Ork Frame Codec — 15B 帧头 + Bootstrap V2（109B）+ Handshake V2（42B）编解码

与 C++ 对齐:
  - 15B 帧头: ver(1) + seq(8 LE) + payload_len(2 LE) + packet_type(4 LE)
  - Bootstrap V2: Envelope(12B) + Payload(97B) = 109B（全网络字节序大端）
      Envelope: magic "ORKB"(4) + version(2 BE)=2 + type(1)=0x01
                + reserved(1)=0 + payloadLength(4 BE)=97
      Payload : connectionId(4 BE) + sessionToken(4 BE) + bootstrapEpoch(16)
                + c2sKey(32) + s2cKey(32) + expireAtMs(8 BE) + flags(1)
  - Handshake V2: 42B 认证握手帧（B1，Rev 3 §5.5/§5.6）
      type(1)=0x03 + version(1)=0x02 + connectionId(4 BE) + clientNonce(16)
      + counter(4 BE) + authTag(16)

本模块是 C++ 唯一权威常量头
source/core/codec/include/ork/codec/bootstrap_protocol_constants.hpp 与
BootstrapFrame（bootstrap_frame.hpp，Rev 3 §5.2/§5.3）、UdpHandshakeV2Frame
（udp_handshake_v2.hpp，Rev 3 §5.5/§5.6）的 Python 镜像：
  - 校验顺序冻结：帧尺寸 → magic → version → type → reserved →
    payloadLength → Payload.flags 保留位；首个失败即停；
  - V1（81B）自 Rev 3 起被拒绝：只编码 / 只接受 109B V2 帧，
    不做 `>=109` 宽容解析；
  - Handshake V1（9B）自 B1 起被拒绝：只编码 / 只验证 42B V2 帧；
  - 固定向量与 C++ bootstrap_v2_test.cpp（§14.2 向量组 A）、
    ork_handshake_v2_test.cpp（向量组 B）逐字节一致。

外层 TCP framing：4B 大端 length prefix = 109，不含 prefix 自身
（validate_bootstrap_outer_length）。
"""

from __future__ import annotations

import hashlib
import hmac
import struct
from enum import IntEnum
from typing import NoReturn

# ── 常量 ──────────────────────────────────────────────────────────────────

ORK_FRAME_HEADER_SIZE = 15  # ver(1) + seq(8 LE) + payload_len(2 LE) + packet_type(4 LE)
CIPHER_VERSION = 0x01

# ── Bootstrap V2 常量（镜像 C++ bootstrap_protocol_constants.hpp §14.1）───

BOOTSTRAP_MAGIC = b"ORKB"
BOOTSTRAP_VERSION = 2
BOOTSTRAP_MESSAGE_TYPE_RESPONSE = 0x01
BOOTSTRAP_RESERVED_VALUE = 0x00
BOOTSTRAP_ENVELOPE_SIZE = 12   # magic(4) + version(2) + type(1) + reserved(1) + payloadLength(4)
BOOTSTRAP_PAYLOAD_SIZE = 97    # connectionId..flags（固定）
BOOTSTRAP_FRAME_SIZE = 109     # Envelope + Payload
BOOTSTRAP_PAYLOAD_LENGTH_VALUE = BOOTSTRAP_PAYLOAD_SIZE
BOOTSTRAP_LENGTH_PREFIX_SIZE = 4  # 外层 4B 大端 prefix（值 = 109，不含 prefix 自身）
BOOTSTRAP_FLAG_ENCRYPTION_REQUIRED = 0x01
BOOTSTRAP_FLAGS_VALID_MASK = BOOTSTRAP_FLAG_ENCRYPTION_REQUIRED

# 字段偏移（帧内绝对偏移；§14.2 向量 hex 布局与之一一对应）
BOOTSTRAP_MAGIC_OFFSET = 0
BOOTSTRAP_VERSION_OFFSET = 4
BOOTSTRAP_TYPE_OFFSET = 6
BOOTSTRAP_RESERVED_OFFSET = 7
BOOTSTRAP_PAYLOAD_LENGTH_OFFSET = 8
BOOTSTRAP_CONNECTION_ID_OFFSET = 12
BOOTSTRAP_SESSION_TOKEN_OFFSET = 16
BOOTSTRAP_EPOCH_OFFSET = 20
BOOTSTRAP_C2S_KEY_OFFSET = 36
BOOTSTRAP_S2C_KEY_OFFSET = 68
BOOTSTRAP_EXPIRE_AT_MS_OFFSET = 100
BOOTSTRAP_FLAGS_OFFSET = 108

# ── Handshake V2 常量（B1，镜像 C++ bootstrap_protocol_constants.hpp §14.1）──

UDP_HANDSHAKE_V2_TYPE = 0x03       # UdpPacketType::HandshakeV2（避让 Handshake=0 / Data=2）
UDP_HANDSHAKE_V2_VERSION = 0x02    # 后续演进递增 version，type 不变
UDP_HANDSHAKE_V2_SIZE = 42         # 1 + 1 + 4 + 16 + 4 + 16
CLIENT_NONCE_SIZE = 16
HANDSHAKE_COUNTER_SIZE = 4
AUTH_TAG_SIZE = 16
HANDSHAKE_KEY_SIZE = 32
SESSION_KEY_SIZE = 32
BOOTSTRAP_EPOCH_SIZE = 16
HANDSHAKE_MAC_LABEL = b"ORK-UDP-HANDSHAKE-V2"   # 20B ASCII 域分隔标签
HANDSHAKE_MAC_INPUT_SIZE = 20 + 1 + 1 + 4 + 16 + 16 + 4  # 62
HANDSHAKE_KEY_INFO_LABEL = b"ORK-UDP-HS-C2S-v2"  # 17B ASCII（HKDF info）
HANDSHAKE_HKDF_SALT = bytes(32)   # 32×0x00（Rev 3 §5.6 冻结）
HANDSHAKE_COUNTER_MAX = 0xFFFF_FFFF  # 溢出 fail closed，不回绕


class FrameType(IntEnum):
    """帧类型，填入 15B 帧头的 packet_type 字段（offset 11，4 字节 LE）"""
    Bootstrap = 0x01      # 服务端 → 客户端：连接建立，携带 BootstrapFrame 载荷
    BootstrapAck = 0x02   # 客户端 → 服务端：确认 bootstrap
    Data = 0x10           # 双向：opaque 游戏数据
    KeepAlive = 0x11      # 双向：心跳
    Disconnect = 0x20     # 服务端 → 客户端：主动断开


# ── 15B OrkFrame 帧头 ────────────────────────────────────────────────────


def encode_ork_frame_header(
    ver: int,
    seq: int,
    payload_len: int,
    packet_type: int,
) -> bytes:
    """编码 15B 帧头: ver(1) + seq(8 LE) + payload_len(2 LE) + packet_type(4 LE)。"""
    if ver < 0 or ver > 0xFF:
        raise ValueError(f"ver out of range: {ver}")
    if seq < 0 or seq > 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError(f"seq out of range: {seq}")
    if payload_len < 0 or payload_len > 0xFFFF:
        raise ValueError(f"payload_len out of range: {payload_len}")
    if packet_type < 0 or packet_type > 0xFFFF_FFFF:
        raise ValueError(f"packet_type out of range: {packet_type}")
    return (
        bytes([ver])
        + struct.pack("<Q", seq)
        + struct.pack("<H", payload_len)
        + struct.pack("<I", packet_type)
    )


def decode_ork_frame_header(data: bytes) -> tuple[int, int, int, int]:
    """解码 15B 帧头 → (ver, seq, payload_len, packet_type)。

    严格检查长度和版本号。
    """
    if len(data) < ORK_FRAME_HEADER_SIZE:
        raise ValueError(f"frame too short: {len(data)} < {ORK_FRAME_HEADER_SIZE}")
    ver = data[0]
    if ver != CIPHER_VERSION:
        raise ValueError(f"unexpected frame version: {ver}")
    seq = struct.unpack("<Q", data[1:9])[0]
    payload_len = struct.unpack("<H", data[9:11])[0]
    packet_type = struct.unpack("<I", data[11:15])[0]
    return ver, seq, payload_len, packet_type


# ── Bootstrap V2（109B）───────────────────────────────────────────────────


def validate_bootstrap_outer_length(declared_length: int) -> bool:
    """外层 4B 大端 length prefix 冻结语义（§5.3 步骤①）：严格等于 109。

    与 C++ `ValidateBootstrapOuterLength` 一一对应；prefix 值不包含
    prefix 自身的 4 字节。
    """
    return declared_length == BOOTSTRAP_FRAME_SIZE


def encode_bootstrap_frame(
    connection_id: int,
    session_token: int,
    bootstrap_epoch: bytes,
    c2s_key: bytes,
    s2c_key: bytes,
    expire_at_ms: int,
    flags: int,
) -> bytes:
    """编码 109B Bootstrap V2 帧（Envelope + Payload，全网络字节序）。

    布局: Envelope(magic + version + type + reserved + payloadLength)
          + connectionId(4 BE) + sessionToken(4 BE) + bootstrapEpoch(16)
          + c2sKey(32) + s2cKey(32) + expireAtMs(8 BE) + flags(1)
    """
    if len(bootstrap_epoch) != 16:
        raise ValueError(f"bootstrap_epoch must be 16 bytes, got {len(bootstrap_epoch)}")
    if len(c2s_key) != 32:
        raise ValueError(f"c2s_key must be 32 bytes, got {len(c2s_key)}")
    if len(s2c_key) != 32:
        raise ValueError(f"s2c_key must be 32 bytes, got {len(s2c_key)}")
    return (
        BOOTSTRAP_MAGIC
        + struct.pack("!H", BOOTSTRAP_VERSION)
        + bytes([BOOTSTRAP_MESSAGE_TYPE_RESPONSE, BOOTSTRAP_RESERVED_VALUE])
        + struct.pack("!I", BOOTSTRAP_PAYLOAD_LENGTH_VALUE)
        + struct.pack("!I", connection_id)
        + struct.pack("!I", session_token)
        + bootstrap_epoch
        + c2s_key
        + s2c_key
        + struct.pack("!Q", expire_at_ms)
        + bytes([flags & 0xFF])
    )


def decode_bootstrap_frame(data: bytes) -> dict:
    """严格解码 109B Bootstrap V2 帧 → dict。

    校验顺序（§5.3 冻结，首个失败即停）：
      帧尺寸 == 109 → magic → version → type → reserved → payloadLength
      → Payload.flags 保留位（合法掩码 0x01）。
    任何失败抛 ValueError（reason 与 C++ BootstrapDecodeError 名称对齐）：
    硬切单轨——不猜 V1、不接受 81B、不按 `>=109` 解码。

    返回: {
        connection_id, session_token, bootstrap_epoch, c2s_key, s2c_key,
        expire_at_ms, flags, encryption_required
    }
    """
    def _reject(reason: str) -> NoReturn:
        raise ValueError(f"bootstrap decode rejected: {reason}")

    if len(data) != BOOTSTRAP_FRAME_SIZE:
        _reject("bad_frame_size")
    if data[BOOTSTRAP_MAGIC_OFFSET:BOOTSTRAP_VERSION_OFFSET] != BOOTSTRAP_MAGIC:
        _reject("bad_magic")
    if struct.unpack(
        "!H", data[BOOTSTRAP_VERSION_OFFSET:BOOTSTRAP_TYPE_OFFSET]
    )[0] != BOOTSTRAP_VERSION:
        _reject("unsupported_version")
    if data[BOOTSTRAP_TYPE_OFFSET] != BOOTSTRAP_MESSAGE_TYPE_RESPONSE:
        _reject("bad_type")
    if data[BOOTSTRAP_RESERVED_OFFSET] != BOOTSTRAP_RESERVED_VALUE:
        _reject("bad_reserved")
    if struct.unpack(
        "!I", data[BOOTSTRAP_PAYLOAD_LENGTH_OFFSET:BOOTSTRAP_CONNECTION_ID_OFFSET]
    )[0] != BOOTSTRAP_PAYLOAD_LENGTH_VALUE:
        _reject("bad_payload_length")

    connection_id = struct.unpack(
        "!I", data[BOOTSTRAP_CONNECTION_ID_OFFSET:BOOTSTRAP_SESSION_TOKEN_OFFSET]
    )[0]
    session_token = struct.unpack(
        "!I", data[BOOTSTRAP_SESSION_TOKEN_OFFSET:BOOTSTRAP_EPOCH_OFFSET]
    )[0]
    bootstrap_epoch = data[BOOTSTRAP_EPOCH_OFFSET:BOOTSTRAP_C2S_KEY_OFFSET]
    c2s_key = data[BOOTSTRAP_C2S_KEY_OFFSET:BOOTSTRAP_S2C_KEY_OFFSET]
    s2c_key = data[BOOTSTRAP_S2C_KEY_OFFSET:BOOTSTRAP_EXPIRE_AT_MS_OFFSET]
    expire_at_ms = struct.unpack(
        "!Q", data[BOOTSTRAP_EXPIRE_AT_MS_OFFSET:BOOTSTRAP_FLAGS_OFFSET]
    )[0]
    flags = data[BOOTSTRAP_FLAGS_OFFSET]

    # Payload 语义深检：flags 保留位必须为 0（M12）
    if flags & ~BOOTSTRAP_FLAGS_VALID_MASK:
        _reject("bad_flags")

    return {
        "connection_id": connection_id,
        "session_token": session_token,
        "bootstrap_epoch": bootstrap_epoch,
        "c2s_key": c2s_key,
        "s2c_key": s2c_key,
        "expire_at_ms": expire_at_ms,
        "flags": flags,
        "encryption_required": bool(flags & BOOTSTRAP_FLAG_ENCRYPTION_REQUIRED),
    }


# ── Handshake V2（42B，B1）────────────────────────────────────────────────


def handshake_counter_can_advance(counter: int) -> bool:
    """counter 是否还能严格递增（溢出 fail closed，不回绕）。"""
    return counter != HANDSHAKE_COUNTER_MAX


def derive_handshake_key(c2s_key: bytes) -> bytes:
    """HKDF-SHA-256 从 c2sKey 派生 32B handshakeKey（Rev 3 §5.6 冻结）。

    salt = 32×0x00，info = "ORK-UDP-HS-C2S-v2"，L = 32（单块 Expand）。
    握手密钥独立于数据通道 nonce/密钥；仅驻留内存，不日志、不序列化。
    """
    if len(c2s_key) != SESSION_KEY_SIZE:
        raise ValueError(f"c2s_key must be {SESSION_KEY_SIZE} bytes, got {len(c2s_key)}")
    prk = hmac.new(HANDSHAKE_HKDF_SALT, c2s_key, hashlib.sha256).digest()
    # 单块 HKDF-Expand：T(32) = HMAC-SHA-256(PRK, info || 0x01)
    okm = hmac.new(prk, HANDSHAKE_KEY_INFO_LABEL + b"\x01", hashlib.sha256).digest()
    return okm[:HANDSHAKE_KEY_SIZE]


def build_handshake_mac_input(
    connection_id: int,
    bootstrap_epoch: bytes,
    client_nonce: bytes,
    counter: int,
) -> bytes:
    """构造 62B MAC 输入（Rev 3 §5.5 冻结布局，全网络字节序）。

    "ORK-UDP-HANDSHAKE-V2"(20 ASCII) || type(1)=0x03 || version(1)=0x02
    || connectionId(4 BE) || bootstrapEpoch(16) || clientNonce(16)
    || counter(4 BE)
    """
    if len(bootstrap_epoch) != BOOTSTRAP_EPOCH_SIZE:
        raise ValueError(
            f"bootstrap_epoch must be {BOOTSTRAP_EPOCH_SIZE} bytes, got {len(bootstrap_epoch)}"
        )
    if len(client_nonce) != CLIENT_NONCE_SIZE:
        raise ValueError(
            f"client_nonce must be {CLIENT_NONCE_SIZE} bytes, got {len(client_nonce)}"
        )
    if not 0 <= counter <= HANDSHAKE_COUNTER_MAX:
        raise ValueError(f"counter out of range: {counter}")
    if not 0 <= connection_id <= 0xFFFF_FFFF:
        raise ValueError(f"connection_id out of range: {connection_id}")
    return (
        HANDSHAKE_MAC_LABEL
        + bytes([UDP_HANDSHAKE_V2_TYPE, UDP_HANDSHAKE_V2_VERSION])
        + struct.pack("!I", connection_id)
        + bootstrap_epoch
        + client_nonce
        + struct.pack("!I", counter)
    )


def compute_handshake_auth_tag(handshake_key: bytes, mac_input: bytes) -> bytes:
    """HMAC-SHA-256(handshakeKey, macInput) 截断前 16B（Rev 3 §5.5）。"""
    if len(handshake_key) != HANDSHAKE_KEY_SIZE:
        raise ValueError(
            f"handshake_key must be {HANDSHAKE_KEY_SIZE} bytes, got {len(handshake_key)}"
        )
    if len(mac_input) != HANDSHAKE_MAC_INPUT_SIZE:
        raise ValueError(
            f"mac_input must be {HANDSHAKE_MAC_INPUT_SIZE} bytes, got {len(mac_input)}"
        )
    return hmac.new(handshake_key, mac_input, hashlib.sha256).digest()[:AUTH_TAG_SIZE]


def encode_handshake_v2_frame(
    connection_id: int,
    client_nonce: bytes,
    counter: int,
    auth_tag: bytes,
) -> bytes:
    """编码 42B Handshake V2 帧（type/version 冻结常量，全网络字节序）。"""
    if len(client_nonce) != CLIENT_NONCE_SIZE:
        raise ValueError(
            f"client_nonce must be {CLIENT_NONCE_SIZE} bytes, got {len(client_nonce)}"
        )
    if len(auth_tag) != AUTH_TAG_SIZE:
        raise ValueError(f"auth_tag must be {AUTH_TAG_SIZE} bytes, got {len(auth_tag)}")
    if not 0 <= counter <= HANDSHAKE_COUNTER_MAX:
        raise ValueError(f"counter out of range: {counter}")
    if not 0 <= connection_id <= 0xFFFF_FFFF:
        raise ValueError(f"connection_id out of range: {connection_id}")
    return (
        bytes([UDP_HANDSHAKE_V2_TYPE, UDP_HANDSHAKE_V2_VERSION])
        + struct.pack("!I", connection_id)
        + client_nonce
        + struct.pack("!I", counter)
        + auth_tag
    )


def build_handshake_v2_packet(
    connection_id: int,
    client_nonce: bytes,
    counter: int,
    bootstrap_epoch: bytes,
    c2s_key: bytes,
) -> bytes:
    """一站式构造 42B 握手包：派生 key → MAC 输入 → authTag → 编码。

    与 C++ BuildHandshakeV2Packet 一一对应；authTag 覆盖除自身外的
    全部 26B 明文字段。
    """
    handshake_key = derive_handshake_key(c2s_key)
    mac_input = build_handshake_mac_input(connection_id, bootstrap_epoch, client_nonce, counter)
    auth_tag = compute_handshake_auth_tag(handshake_key, mac_input)
    return encode_handshake_v2_frame(connection_id, client_nonce, counter, auth_tag)


def decode_handshake_v2_frame(data: bytes) -> dict:
    """严格解码 42B Handshake V2 帧 → dict。

    校验顺序（首个失败即停）：帧尺寸 == 42 → type == 0x03 → version == 0x02。
    失败抛 ValueError（reason 与 C++ HandshakeV2DecodeError 名称对齐）；
    硬切单轨——只验证 V2，不做 V1 嗅探。

    返回: {connection_id, client_nonce, counter, auth_tag}
    """
    def _reject(reason: str) -> NoReturn:
        raise ValueError(f"handshake v2 decode rejected: {reason}")

    if len(data) != UDP_HANDSHAKE_V2_SIZE:
        _reject("bad_frame_size")
    if data[0] != UDP_HANDSHAKE_V2_TYPE:
        _reject("bad_type")
    if data[1] != UDP_HANDSHAKE_V2_VERSION:
        _reject("unsupported_version")

    return {
        "connection_id": struct.unpack("!I", data[2:6])[0],
        "client_nonce": data[6:22],
        "counter": struct.unpack("!I", data[22:26])[0],
        "auth_tag": data[26:42],
    }


def verify_handshake_v2_auth_tag(
    data: bytes,
    handshake_key: bytes,
    bootstrap_epoch: bytes,
) -> bool:
    """验证 42B 帧的 authTag（常量时间比较）。

    结构校验失败直接 False（服务端静默丢弃语义，不抛异常）；
    重算 expected tag 与帧内 authTag 用 hmac.compare_digest 常量时间比对。
    """
    try:
        frame = decode_handshake_v2_frame(data)
    except ValueError:
        return False
    mac_input = build_handshake_mac_input(
        frame["connection_id"], bootstrap_epoch, frame["client_nonce"], frame["counter"]
    )
    expected = compute_handshake_auth_tag(handshake_key, mac_input)
    return hmac.compare_digest(expected, frame["auth_tag"])
