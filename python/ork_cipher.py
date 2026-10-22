"""
Ork Cipher v1 — Python 参考实现（与 C++ ork_cipher.cpp 对齐）

用于压测工具链（benchmark/stress 的 proto-rich 客户端）联调；依赖 PyNaCl (libsodium)。
规范：source/core/crypto/src/OrkTransportCipher_v1.md §22

Phase 3.5: 明文模式也保留 15B 帧头（与 C++ Seal/Open 一致）。
"""

from __future__ import annotations

from dataclasses import dataclass, field
import time
from typing import Optional

from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_decrypt,
    crypto_aead_chacha20poly1305_ietf_encrypt,
)

FRAME_HEADER_SIZE = 15  # ver(1)+seq(8)+payload_len(2)+packet_type(4)
CIPHER_VERSION = 0x01
UDP_REPLAY_WINDOW = 256

C2S = 0x00
S2C = 0x01
CHANNEL_RUDP = 0x01  # 概念上称为 RUDP（Reliable UDP），底层使用 KCP 库
CHANNEL_KCP = CHANNEL_RUDP  # 向后兼容别名
CHANNEL_UDP = 0x02


@dataclass
class ChannelCipherState:
    tx_seq: int = 1
    rudp_last_accepted_seq: int = 0  # 概念上称为 RUDP
    udp_highest_seq: int = 0
    udp_seen: int = 0


@dataclass
class CipherState:
    c2s_key: bytes
    s2c_key: bytes
    connection_id: int
    session_token: int
    expire_at: int = 0
    encryption_required: bool = False
    rudp: ChannelCipherState = field(default_factory=ChannelCipherState)  # 概念上称为 RUDP
    udp: ChannelCipherState = field(default_factory=ChannelCipherState)

    @classmethod
    def from_assign(cls, assign_msg) -> "CipherState":
        """从 AssignConnection protobuf 消息构建（已废弃，保留兼容）。"""
        return cls(
            c2s_key=bytes(assign_msg.c2s_key),
            s2c_key=bytes(assign_msg.s2c_key),
            connection_id=assign_msg.connection_id,
            session_token=assign_msg.session_token,
            expire_at=assign_msg.expire_at,
            encryption_required=assign_msg.encryption_required,
        )

    @classmethod
    def from_bootstrap(cls, bootstrap: dict) -> "CipherState":
        """从 BootstrapFrame dict 构建（Phase 3.5 opaque 协议）。"""
        return cls(
            c2s_key=bytes(bootstrap["c2s_key"]),
            s2c_key=bytes(bootstrap["s2c_key"]),
            connection_id=bootstrap["connection_id"],
            session_token=bootstrap["session_token"],
            expire_at=bootstrap["expire_at_ms"],
            encryption_required=bootstrap["encryption_required"],
        )

    @classmethod
    def from_keys(
        cls,
        connection_id: int,
        session_token: int,
        c2s_key: bytes,
        s2c_key: bytes,
        *,
        expire_at: int = 0,
        encryption_required: bool = True,
    ) -> "CipherState":
        """从原始密钥和连接参数构建。"""
        return cls(
            c2s_key=bytes(c2s_key),
            s2c_key=bytes(s2c_key),
            connection_id=connection_id,
            session_token=session_token,
            expire_at=expire_at,
            encryption_required=encryption_required,
        )


def make_nonce(version: int, direction: int, channel: int, seq: int) -> bytes:
    return bytes([version, direction, channel, 0x00]) + seq.to_bytes(8, "big")


def make_aad(
    connection_id: int,
    session_token: int,
    packet_type: int,
    channel: int,
    seq: int,
    payload_len: int,
) -> bytes:
    return (
        b"ORK1"
        + bytes([CIPHER_VERSION, channel])
        + connection_id.to_bytes(4, "big")
        + session_token.to_bytes(4, "big")
        + packet_type.to_bytes(4, "big")
        + seq.to_bytes(8, "big")
        + payload_len.to_bytes(2, "big")
    )


def _select_seal_key(state: CipherState, direction: int) -> bytes:
    return state.c2s_key if direction == C2S else state.s2c_key


def _select_open_key(state: CipherState, peer_direction: int) -> bytes:
    return state.c2s_key if peer_direction == C2S else state.s2c_key


def _channel_state(state: CipherState, channel: int) -> ChannelCipherState:
    return state.rudp if channel == CHANNEL_RUDP else state.udp


def _udp_replay_is_seen(window: ChannelCipherState, seq: int) -> bool:
    if window.udp_highest_seq == 0 or seq > window.udp_highest_seq:
        return False
    if window.udp_highest_seq - seq >= UDP_REPLAY_WINDOW:
        return True
    return bool(window.udp_seen & (1 << (seq % UDP_REPLAY_WINDOW)))


def udp_cheap_filter(window: ChannelCipherState, seq: int) -> bool:
    if seq == 0:
        return False
    if window.udp_highest_seq > 0 and seq + UDP_REPLAY_WINDOW <= window.udp_highest_seq:
        return False
    if seq <= window.udp_highest_seq and _udp_replay_is_seen(window, seq):
        return False
    return True


def commit_udp_replay(window: ChannelCipherState, seq: int) -> None:
    if seq == 0:
        return
    if seq > window.udp_highest_seq:
        if window.udp_highest_seq == 0 or seq - window.udp_highest_seq >= UDP_REPLAY_WINDOW:
            window.udp_seen = 0
        else:
            for i in range(window.udp_highest_seq + 1, seq + 1):
                window.udp_seen &= ~(1 << (i % UDP_REPLAY_WINDOW))
        window.udp_highest_seq = seq
    window.udp_seen |= 1 << (seq % UDP_REPLAY_WINDOW)


def accept_rudp_replay(window: ChannelCipherState, seq: int) -> bool:
    """RUDP（概念称呼）重放检测。底层使用 KCP 库。"""
    if seq == 0 or seq <= window.rudp_last_accepted_seq:
        return False
    window.rudp_last_accepted_seq = seq
    return True


def seal(
    state: CipherState,
    channel: int,
    direction: int,
    packet_type: int,
    plaintext: bytes,
) -> Optional[bytes]:
    if len(plaintext) > 0xFFFF:
        return None

    ch = _channel_state(state, channel)
    seq = ch.tx_seq
    ch.tx_seq += 1

    # Phase 3.5: 明文模式保留 15B 帧头（与 C++ OrkCipher::Seal 一致）
    if not state.encryption_required:
        return (
            bytes([CIPHER_VERSION])
            + seq.to_bytes(8, "little")
            + len(plaintext).to_bytes(2, "little")
            + packet_type.to_bytes(4, "little")
            + plaintext
        )

    key = _select_seal_key(state, direction)
    nonce = make_nonce(CIPHER_VERSION, direction, channel, seq)
    aad = make_aad(
        state.connection_id,
        state.session_token,
        packet_type,
        channel,
        seq,
        len(plaintext),
    )
    sealed = crypto_aead_chacha20poly1305_ietf_encrypt(plaintext, aad, nonce, key)
    return (
        bytes([CIPHER_VERSION])
        + seq.to_bytes(8, "little")
        + len(plaintext).to_bytes(2, "little")
        + packet_type.to_bytes(4, "little")
        + sealed
    )


def open_frame(
    state: CipherState,
    channel: int,
    peer_direction: int,
    packet_type: int,
    frame: bytes,
) -> Optional[bytes]:
    if not state.encryption_required:
        # Phase 3.5: 明文模式解析 15B 帧头，更新 seq/replay，返回去掉帧头的载荷
        return _open_plaintext_frame(state, channel, frame)

    if state.expire_at > 0 and int(time.time() * 1000) > state.expire_at:
        return None
    if len(frame) < FRAME_HEADER_SIZE + 16 or frame[0] != CIPHER_VERSION:
        return None

    seq = int.from_bytes(frame[1:9], "little")
    payload_len = int.from_bytes(frame[9:11], "little")
    frame_packet_type = int.from_bytes(frame[11:15], "little")
    sealed = frame[FRAME_HEADER_SIZE:]

    aad_packet_type = packet_type if packet_type != 0 else frame_packet_type
    if packet_type != 0 and packet_type != frame_packet_type:
        return None

    ch = _channel_state(state, channel)
    if channel == CHANNEL_UDP and not udp_cheap_filter(ch, seq):
        return None

    key = _select_open_key(state, peer_direction)
    nonce = make_nonce(CIPHER_VERSION, peer_direction, channel, seq)
    aad = make_aad(
        state.connection_id,
        state.session_token,
        aad_packet_type,
        channel,
        seq,
        payload_len,
    )

    try:
        plain = crypto_aead_chacha20poly1305_ietf_decrypt(sealed, aad, nonce, key)
    except Exception:
        return None

    if len(plain) != payload_len:
        return None

    if channel == CHANNEL_RUDP:
        if not accept_rudp_replay(ch, seq):
            return None
    else:
        commit_udp_replay(ch, seq)

    return plain


def _open_plaintext_frame(
    state: CipherState,
    channel: int,
    frame: bytes,
) -> Optional[bytes]:
    """明文模式：解析 15B 帧头，更新 seq/replay，返回去掉帧头的载荷。

    与 C++ OrkCipher::Open 明文分支对齐。
    """
    if len(frame) < FRAME_HEADER_SIZE:
        return None
    if frame[0] != CIPHER_VERSION:
        return None

    seq = int.from_bytes(frame[1:9], "little")
    payload_len = int.from_bytes(frame[9:11], "little")
    if len(frame) < FRAME_HEADER_SIZE + payload_len:
        return None

    ch = _channel_state(state, channel)
    if channel == CHANNEL_RUDP:
        if seq == 0 or seq <= ch.rudp_last_accepted_seq:
            return None
        ch.rudp_last_accepted_seq = seq
    else:
        if not udp_cheap_filter(ch, seq):
            return None
        commit_udp_replay(ch, seq)

    return frame[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + payload_len]


def _smoke_test() -> None:
    """与 ork_cipher_test.cpp MakeTestState 对齐的本地自测。"""
    state = CipherState(
        c2s_key=bytes([0x11] * 32),
        s2c_key=bytes([0x22] * 32),
        connection_id=0x12345678,
        session_token=0xAABBCCDD,
        encryption_required=True,
    )
    plain = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02])
    frame = seal(state, CHANNEL_RUDP, C2S, 5, plain)
    assert frame is not None
    opened = open_frame(state, CHANNEL_RUDP, C2S, 0, frame)
    assert opened == plain
    print("ork_cipher.py smoke test passed")


if __name__ == "__main__":
    _smoke_test()
