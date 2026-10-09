"""在线测试 — 验证加密通信（ChaCha20-Poly1305-IETF AEAD 数据通道）。

demo 服务端 Bootstrap flags 置 encryption_required，客户端 CipherState 自动进入
AEAD 模式：所有上行 seal 加密、下行 open 解密 + 完整性校验。本测试证明：
- 会话协商出加密模式（encryption_required=True）
- 双通道 echo 均经真实解密 + Poly1305 校验通过（否则回显会被丢弃、计数为 0）
- 用会话真实协商密钥做本地 seal→open 往返，且篡改密文被拒
- 全程无 open/decrypt 失败错误
"""

from __future__ import annotations

import pytest

from lib import ork_cipher as C
from lib.demo_helpers import (
    RELIABLE_MARKER,
    UNRELIABLE_MARKER,
    connect_verbose,
    echo_once_reliable,
    echo_once_unreliable,
    marker_int,
)


@pytest.fixture
def session(server, host, port):
    s = connect_verbose(host, port, "test-encryption", quiet=True)
    try:
        yield s
    finally:
        s.close()


def test_encryption_negotiated(session):
    assert session.encryption_required is True, "服务端应协商出加密模式"
    assert session.cipher is not None
    assert session.cipher.encryption_required is True
    assert len(session.cipher.c2s_key) == 32
    assert len(session.cipher.s2c_key) == 32


def test_encrypted_reliable_echo(session):
    seq = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF
    assert echo_once_reliable(session, seq, timeout=5.0) is not None
    assert session.stats.bench_rudp_echoes >= 1
    assert not [e for e in session.stats.errors if "decrypt" in e or "open" in e]


def test_encrypted_unreliable_echo(session):
    seq = marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF
    assert echo_once_unreliable(session, seq, timeout=1.5, attempts=3) is not None
    assert session.stats.bench_udp_echoes >= 1


def test_session_keys_seal_open_roundtrip(session):
    """用会话真实协商密钥做本地 AEAD 往返 + 篡改拒绝。"""
    keys = session.cipher
    local = C.CipherState.from_keys(
        connection_id=keys.connection_id,
        session_token=keys.session_token,
        c2s_key=keys.c2s_key,
        s2c_key=keys.s2c_key,
        encryption_required=True,
    )
    plaintext = b"ORK-AEAD-SELFTEST-PAYLOAD"
    frame = C.seal(local, C.CHANNEL_RUDP, C.C2S, 5, plaintext)
    assert frame is not None
    assert frame[0] == C.CIPHER_VERSION
    assert frame[C.FRAME_HEADER_SIZE:] != plaintext  # 确已加密（非明文透传）

    rx = C.CipherState.from_keys(
        connection_id=keys.connection_id,
        session_token=keys.session_token,
        c2s_key=keys.c2s_key,
        s2c_key=keys.s2c_key,
        encryption_required=True,
    )
    assert C.open_frame(rx, C.CHANNEL_RUDP, C.C2S, 0, frame) == plaintext

    tampered = bytearray(frame)
    tampered[-1] ^= 0xFF
    rx2 = C.CipherState.from_keys(
        connection_id=keys.connection_id,
        session_token=keys.session_token,
        c2s_key=keys.c2s_key,
        s2c_key=keys.s2c_key,
        encryption_required=True,
    )
    assert C.open_frame(rx2, C.CHANNEL_RUDP, C.C2S, 0, bytes(tampered)) is None
