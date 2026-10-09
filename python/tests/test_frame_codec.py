"""离线单元测试 — lib/ork_frame.py（不连服务端）。

覆盖：
- 15B OrkFrame 帧头编解码 + 字段偏移
- Bootstrap V2（109B）编码匹配已知向量 / 解码往返 / 字段偏移 / 畸形拒绝
- Handshake V2（42B）构造匹配已知向量 / authTag 验证 / 篡改失败 / 硬切拒绝
- 与 testdata/golden/*.hex 交叉校验（golden 由 testdata/generate.py 冻结）
"""

from __future__ import annotations

from pathlib import Path

import pytest

from lib import ork_frame as F


# ── 已知向量（与 C++ bootstrap_v2_test.cpp / ork_handshake_v2_test.cpp 对齐）──
VECTOR_A_HEX = (
    "4F524B4200020100000000610000000ADEADBEEF000102030405060708090A0B0C0D0E0F"
    "202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F"
    "404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F"
    "000001A0C4506C0001"
)
HANDSHAKE_COUNTER0_HEX = (
    "03020000000A101112131415161718191A1B1C1D1E1F00000000177B22F12707695BBA6212D69EB54033"
)
HANDSHAKE_COUNTER1_HEX = (
    "03020000000A101112131415161718191A1B1C1D1E1F00000001D3D3019CAB1E5A77A52F030AA39B112F"
)
HANDSHAKE_KEY_HEX = "4A7B7076F11E316A6D3B3F5D716AEE01D7E6FC536A05A562877F250D2E3DC4DB"

VB_C2S_KEY = bytes(range(0x20, 0x40))
VB_EPOCH = bytes(range(16))
VB_NONCE = bytes(range(0x10, 0x20))


def _bootstrap_fields() -> dict:
    return dict(
        connection_id=10,
        session_token=0xDEADBEEF,
        bootstrap_epoch=bytes(range(16)),
        c2s_key=bytes(range(0x20, 0x40)),
        s2c_key=bytes(range(0x40, 0x60)),
        expire_at_ms=1790000000000,
        flags=0x01,
    )


# ── 15B 帧头 ────────────────────────────────────────────────────────────────


def test_frame_header_roundtrip():
    hdr = F.encode_ork_frame_header(0x01, 42, 100, int(F.FrameType.Data))
    assert len(hdr) == F.ORK_FRAME_HEADER_SIZE == 15
    assert F.decode_ork_frame_header(hdr) == (0x01, 42, 100, int(F.FrameType.Data))


def test_frame_header_field_offsets():
    """ver(1)@0 + seq(8 LE)@1 + payload_len(2 LE)@9 + packet_type(4 LE)@11。"""
    hdr = F.encode_ork_frame_header(0x01, 0x0102030405060708, 0x0A0B, 0x0C0D0E0F)
    assert hdr[0] == 0x01
    assert hdr[1:9] == (0x0102030405060708).to_bytes(8, "little")
    assert hdr[9:11] == (0x0A0B).to_bytes(2, "little")
    assert hdr[11:15] == (0x0C0D0E0F).to_bytes(4, "little")


def test_frame_header_range_validation():
    with pytest.raises(ValueError):
        F.encode_ork_frame_header(0x100, 0, 0, 0)      # ver 越界
    with pytest.raises(ValueError):
        F.encode_ork_frame_header(0x01, -1, 0, 0)       # seq 越界
    with pytest.raises(ValueError):
        F.decode_ork_frame_header(b"\x01" * 10)          # 太短


# ── Bootstrap V2（109B）─────────────────────────────────────────────────────


def test_bootstrap_encode_matches_known_vector():
    assert F.encode_bootstrap_frame(**_bootstrap_fields()) == bytes.fromhex(VECTOR_A_HEX)


def test_bootstrap_frame_size_and_prefix():
    bf = F.encode_bootstrap_frame(**_bootstrap_fields())
    assert len(bf) == F.BOOTSTRAP_FRAME_SIZE == 109
    assert bf[:4] == F.BOOTSTRAP_MAGIC


def test_bootstrap_field_offsets():
    bf = F.encode_bootstrap_frame(**_bootstrap_fields())
    f = _bootstrap_fields()
    assert int.from_bytes(bf[F.BOOTSTRAP_CONNECTION_ID_OFFSET:F.BOOTSTRAP_SESSION_TOKEN_OFFSET], "big") == f["connection_id"]
    assert int.from_bytes(bf[F.BOOTSTRAP_SESSION_TOKEN_OFFSET:F.BOOTSTRAP_EPOCH_OFFSET], "big") == f["session_token"]
    assert bf[F.BOOTSTRAP_EPOCH_OFFSET:F.BOOTSTRAP_C2S_KEY_OFFSET] == f["bootstrap_epoch"]
    assert bf[F.BOOTSTRAP_C2S_KEY_OFFSET:F.BOOTSTRAP_S2C_KEY_OFFSET] == f["c2s_key"]
    assert bf[F.BOOTSTRAP_S2C_KEY_OFFSET:F.BOOTSTRAP_EXPIRE_AT_MS_OFFSET] == f["s2c_key"]
    assert int.from_bytes(bf[F.BOOTSTRAP_EXPIRE_AT_MS_OFFSET:F.BOOTSTRAP_FLAGS_OFFSET], "big") == f["expire_at_ms"]
    assert bf[F.BOOTSTRAP_FLAGS_OFFSET] == f["flags"]


def test_bootstrap_decode_roundtrip():
    bf = F.encode_bootstrap_frame(**_bootstrap_fields())
    parsed = F.decode_bootstrap_frame(bf)
    assert parsed["connection_id"] == 10
    assert parsed["session_token"] == 0xDEADBEEF
    assert parsed["c2s_key"] == bytes(range(0x20, 0x40))
    assert parsed["encryption_required"] is True


def test_bootstrap_outer_length_semantics():
    assert F.validate_bootstrap_outer_length(109)
    assert not F.validate_bootstrap_outer_length(81)


@pytest.mark.parametrize("mutant,reason", [
    (b"ORKX" + bytes.fromhex(VECTOR_A_HEX)[4:], "bad_magic"),
    (bytes.fromhex(VECTOR_A_HEX)[:4] + b"\x00\x01" + bytes.fromhex(VECTOR_A_HEX)[6:], "unsupported_version"),
    (bytes.fromhex(VECTOR_A_HEX)[:6] + b"\x02" + bytes.fromhex(VECTOR_A_HEX)[7:], "bad_type"),
    (bytes.fromhex(VECTOR_A_HEX)[:7] + b"\x01" + bytes.fromhex(VECTOR_A_HEX)[8:], "bad_reserved"),
    (bytes.fromhex(VECTOR_A_HEX)[:11] + b"\x60" + bytes.fromhex(VECTOR_A_HEX)[12:], "bad_payload_length"),
    (bytes.fromhex(VECTOR_A_HEX)[:108] + b"\x02", "bad_flags"),
])
def test_bootstrap_rejects_malformed(mutant, reason):
    with pytest.raises(ValueError) as exc:
        F.decode_bootstrap_frame(mutant)
    assert reason in str(exc.value)


@pytest.mark.parametrize("blob", [
    bytes.fromhex(VECTOR_A_HEX)[:81],
    bytes.fromhex(VECTOR_A_HEX)[:108],
    bytes.fromhex(VECTOR_A_HEX) + b"\x00",
])
def test_bootstrap_rejects_bad_size(blob):
    with pytest.raises(ValueError) as exc:
        F.decode_bootstrap_frame(blob)
    assert "bad_frame_size" in str(exc.value)


# ── Handshake V2（42B）──────────────────────────────────────────────────────


def test_handshake_key_derivation():
    assert F.derive_handshake_key(VB_C2S_KEY) == bytes.fromhex(HANDSHAKE_KEY_HEX)


def test_handshake_build_matches_known_vectors():
    p0 = F.build_handshake_v2_packet(10, VB_NONCE, 0, VB_EPOCH, VB_C2S_KEY)
    p1 = F.build_handshake_v2_packet(10, VB_NONCE, 1, VB_EPOCH, VB_C2S_KEY)
    assert len(p0) == F.UDP_HANDSHAKE_V2_SIZE == 42
    assert p0 == bytes.fromhex(HANDSHAKE_COUNTER0_HEX)
    assert p1 == bytes.fromhex(HANDSHAKE_COUNTER1_HEX)


def test_handshake_mac_input_layout():
    mac = F.build_handshake_mac_input(10, VB_EPOCH, VB_NONCE, 0)
    assert mac == (b"ORK-UDP-HANDSHAKE-V2" + b"\x03\x02" + b"\x00\x00\x00\x0A"
                   + VB_EPOCH + VB_NONCE + b"\x00\x00\x00\x00")
    assert len(mac) == F.HANDSHAKE_MAC_INPUT_SIZE == 62


def test_handshake_verify_and_tamper():
    key = F.derive_handshake_key(VB_C2S_KEY)
    p0 = bytes.fromhex(HANDSHAKE_COUNTER0_HEX)
    assert F.verify_handshake_v2_auth_tag(p0, key, VB_EPOCH)
    tampered = bytearray(p0)
    tampered[10] ^= 0x01
    assert not F.verify_handshake_v2_auth_tag(bytes(tampered), key, VB_EPOCH)
    assert not F.verify_handshake_v2_auth_tag(p0, bytes(range(0xA0, 0xC0)), VB_EPOCH)


def test_handshake_decode_and_hard_reject():
    p0 = bytes.fromhex(HANDSHAKE_COUNTER0_HEX)
    frame = F.decode_handshake_v2_frame(p0)
    assert frame["connection_id"] == 10
    assert frame["counter"] == 0
    with pytest.raises(ValueError) as exc:
        F.decode_handshake_v2_frame(p0[:41])
    assert "bad_frame_size" in str(exc.value)
    with pytest.raises(ValueError) as exc:
        F.decode_handshake_v2_frame(bytes([0x00]) + p0[1:])
    assert "bad_type" in str(exc.value)


def test_counter_overflow_fail_closed():
    assert F.handshake_counter_can_advance(0)
    assert not F.handshake_counter_can_advance(F.HANDSHAKE_COUNTER_MAX)


# ── golden 交叉校验（testdata/golden/*.hex 存在时）──────────────────────────


def _read_golden(name: str) -> bytes | None:
    p = Path(__file__).resolve().parent.parent / "testdata" / "golden" / name
    if not p.is_file():
        return None
    return bytes.fromhex("".join(p.read_text().split()))


def test_golden_bootstrap_matches():
    g = _read_golden("bootstrap_v2.hex")
    if g is None:
        pytest.skip("golden/bootstrap_v2.hex 不存在")
    assert F.encode_bootstrap_frame(**_bootstrap_fields()) == g


def test_golden_handshake_matches():
    g0 = _read_golden("handshake_v2_counter0.hex")
    g1 = _read_golden("handshake_v2_counter1.hex")
    if g0 is None or g1 is None:
        pytest.skip("golden/handshake_v2_*.hex 不存在")
    assert F.build_handshake_v2_packet(10, VB_NONCE, 0, VB_EPOCH, VB_C2S_KEY) == g0
    assert F.build_handshake_v2_packet(10, VB_NONCE, 1, VB_EPOCH, VB_C2S_KEY) == g1
