"""离线单元测试 — lib/ork_cipher.py（ChaCha20-Poly1305-IETF AEAD，不连服务端）。

覆盖：
- seal → open 往返（RUDP / UDP，加密模式）
- 明文模式保留 15B 帧头且可 open
- 帧头字段解析（ver / seq / payload_len / packet_type）
- nonce 确定性 + 域分隔（direction / channel / seq 任一不同即不同 nonce）
- AAD 域分隔
- 重放检测：RUDP seq 必须严格递增；UDP 窗口内重复被拒
- 完整性：篡改密文 → open 返回 None
- 与 testdata/golden/cipher_seal.hex 交叉校验

所有密钥 / 明文 / payload 模式统一来自 tests/vectors.py（单一来源）。
"""

from __future__ import annotations

import pytest

import vectors as V
from lib import ork_cipher as C


def _state(*, encryption_required: bool = True) -> C.CipherState:
    return C.CipherState(
        c2s_key=V.CIPHER_C2S_KEY,
        s2c_key=V.CIPHER_S2C_KEY,
        connection_id=V.CIPHER_CONNECTION_ID,
        session_token=V.CIPHER_SESSION_TOKEN,
        encryption_required=encryption_required,
    )


PLAIN = V.CIPHER_PLAIN


# ── seal / open 往返 ────────────────────────────────────────────────────────


@pytest.mark.parametrize("channel", [C.CHANNEL_RUDP, C.CHANNEL_UDP])
def test_seal_open_roundtrip(channel):
    st = _state()
    frame = C.seal(st, channel, C.C2S, 5, PLAIN)
    assert frame is not None
    # 收方用对端方向 S2C? 不：自测里发送方=C2S，回环 open 用 peer_direction=C2S
    opened = C.open_frame(_state(), channel, C.C2S, 0, frame)
    assert opened == PLAIN


def test_seal_frame_header_layout():
    st = _state()
    frame = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    assert frame[0] == C.CIPHER_VERSION == 0x01
    seq = int.from_bytes(frame[1:9], "little")
    payload_len = int.from_bytes(frame[9:11], "little")
    packet_type = int.from_bytes(frame[11:15], "little")
    assert seq == 1                       # tx_seq 从 1 起
    assert payload_len == len(PLAIN)      # 明文长度（不含 tag）
    assert packet_type == 5
    # 密文体 = AEAD(明文+16B tag)
    assert len(frame) == C.FRAME_HEADER_SIZE + len(PLAIN) + 16


def test_seal_seq_increments_per_channel():
    st = _state()
    f1 = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    f2 = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    assert int.from_bytes(f1[1:9], "little") == 1
    assert int.from_bytes(f2[1:9], "little") == 2
    # 另一通道独立计数
    fu = C.seal(st, C.CHANNEL_UDP, C.C2S, 5, PLAIN)
    assert int.from_bytes(fu[1:9], "little") == 1


# ── 明文模式 ────────────────────────────────────────────────────────────────


def test_plaintext_mode_keeps_header_and_opens():
    st = _state(encryption_required=False)
    frame = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    assert frame[0] == C.CIPHER_VERSION
    assert frame[C.FRAME_HEADER_SIZE:] == PLAIN       # 明文模式：无密文，直接跟明文
    opened = C.open_frame(_state(encryption_required=False), C.CHANNEL_RUDP, C.C2S, 0, frame)
    assert opened == PLAIN


# ── nonce / AAD 域分隔 ──────────────────────────────────────────────────────


def test_nonce_deterministic_and_domain_separated():
    n = C.make_nonce(C.CIPHER_VERSION, C.C2S, C.CHANNEL_RUDP, 7)
    assert n == C.make_nonce(C.CIPHER_VERSION, C.C2S, C.CHANNEL_RUDP, 7)
    assert len(n) == 12
    # direction 不同
    assert n != C.make_nonce(C.CIPHER_VERSION, C.S2C, C.CHANNEL_RUDP, 7)
    # channel 不同
    assert n != C.make_nonce(C.CIPHER_VERSION, C.C2S, C.CHANNEL_UDP, 7)
    # seq 不同
    assert n != C.make_nonce(C.CIPHER_VERSION, C.C2S, C.CHANNEL_RUDP, 8)


def test_aad_domain_separation():
    base = C.make_aad(1, 2, 5, C.CHANNEL_RUDP, 9, 6)
    assert base.startswith(b"ORK1")
    assert base != C.make_aad(1, 2, 5, C.CHANNEL_UDP, 9, 6)     # channel
    assert base != C.make_aad(1, 2, 6, C.CHANNEL_RUDP, 9, 6)     # packet_type
    assert base != C.make_aad(1, 3, 5, C.CHANNEL_RUDP, 9, 6)     # token
    assert base != C.make_aad(2, 2, 5, C.CHANNEL_RUDP, 9, 6)     # conn


# ── 重放检测 ────────────────────────────────────────────────────────────────


def test_rudp_replay_requires_strictly_increasing_seq():
    st = _state()
    w = st.rudp
    assert C.accept_rudp_replay(w, 1) is True
    assert C.accept_rudp_replay(w, 1) is False    # 重复
    assert C.accept_rudp_replay(w, 0) is False    # 0 非法
    assert C.accept_rudp_replay(w, 2) is True     # 递增 OK
    assert C.accept_rudp_replay(w, 2) is False


def test_rudp_open_rejects_replayed_frame():
    st_tx = _state()
    frame = C.seal(st_tx, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    rx = _state()
    assert C.open_frame(rx, C.CHANNEL_RUDP, C.C2S, 0, frame) == PLAIN
    # 同一帧二次 open（seq 未前进）→ 被重放保护拒绝
    assert C.open_frame(rx, C.CHANNEL_RUDP, C.C2S, 0, frame) is None


def test_udp_replay_window():
    w = C.ChannelCipherState()
    assert C.udp_cheap_filter(w, 5) is True
    C.commit_udp_replay(w, 5)
    assert C.udp_cheap_filter(w, 5) is False      # 窗口内已见 → 拒
    assert C.udp_cheap_filter(w, 6) is True       # 前进 OK
    assert C.udp_cheap_filter(w, 0) is False      # seq 0 非法
    # 远超出窗口（旧包）→ 拒
    C.commit_udp_replay(w, 5 + C.UDP_REPLAY_WINDOW + 10)
    assert C.udp_cheap_filter(w, 5) is False


# ── 完整性 ──────────────────────────────────────────────────────────────────


def test_tampered_ciphertext_rejected():
    st = _state()
    frame = bytearray(C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN))
    frame[-1] ^= 0x01                             # 翻转 tag 末字节
    assert C.open_frame(_state(), C.CHANNEL_RUDP, C.C2S, 0, bytes(frame)) is None


def test_open_rejects_bad_version_and_short_frame():
    assert C.open_frame(_state(), C.CHANNEL_RUDP, C.C2S, 0, b"\x02" + b"\x00" * 40) is None
    assert C.open_frame(_state(), C.CHANNEL_RUDP, C.C2S, 0, b"\x01\x00") is None


# ── golden 交叉校验 ─────────────────────────────────────────────────────────


def test_golden_cipher_seal_matches(testdata_dir):
    p = testdata_dir / "golden" / "cipher_seal.hex"
    if not p.is_file():
        pytest.skip("golden/cipher_seal.hex 不存在")
    golden = bytes.fromhex("".join(p.read_text().split()))
    st = _state()
    frame = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, PLAIN)
    assert frame == golden


# ── testdata/payloads 各尺寸 seal→open 往返 ─────────────────────────────────


@pytest.mark.parametrize("size", V.PAYLOAD_SIZES)
def test_payload_files_roundtrip(testdata_dir, size):
    p = testdata_dir / "payloads" / f"payload_{size}.bin"
    if not p.is_file():
        pytest.skip(f"payloads/payload_{size}.bin 不存在")
    data = p.read_bytes()
    assert len(data) == size
    # 可预测模式：第 i 字节 = (i*31+7)&0xFF
    assert data == V.payload_pattern(size)
    tx = _state()
    frame = C.seal(tx, C.CHANNEL_RUDP, C.C2S, 5, data)
    assert frame is not None
    assert C.open_frame(_state(), C.CHANNEL_RUDP, C.C2S, 0, frame) == data
