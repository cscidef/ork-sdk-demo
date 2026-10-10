"""离线单元测试 — lib/ork_frame.py（不连服务端）。

覆盖：
- 15B OrkFrame 帧头编解码 + 字段偏移
- Bootstrap V2（109B）编码匹配已知向量 / 解码往返 / 字段偏移 / 畸形拒绝
- Handshake V2（42B）构造匹配已知向量 / authTag 验证 / 篡改失败 / 硬切拒绝
- 与 testdata/golden/*.hex 交叉校验（golden 由 testdata/generate.py 冻结）

所有输入参数与已知答案 hex 统一来自 tests/vectors.py（单一来源）。
"""

from __future__ import annotations

import pytest

import vectors as V
from lib import ork_frame as F


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
    assert F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS) == bytes.fromhex(V.VECTOR_A_HEX)


def test_bootstrap_frame_size_and_prefix():
    bf = F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS)
    assert len(bf) == F.BOOTSTRAP_FRAME_SIZE == 109
    assert bf[:4] == F.BOOTSTRAP_MAGIC


def test_bootstrap_field_offsets():
    bf = F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS)
    f = V.BOOTSTRAP_INPUTS
    assert int.from_bytes(bf[F.BOOTSTRAP_CONNECTION_ID_OFFSET:F.BOOTSTRAP_SESSION_TOKEN_OFFSET], "big") == f["connection_id"]
    assert int.from_bytes(bf[F.BOOTSTRAP_SESSION_TOKEN_OFFSET:F.BOOTSTRAP_EPOCH_OFFSET], "big") == f["session_token"]
    assert bf[F.BOOTSTRAP_EPOCH_OFFSET:F.BOOTSTRAP_C2S_KEY_OFFSET] == f["bootstrap_epoch"]
    assert bf[F.BOOTSTRAP_C2S_KEY_OFFSET:F.BOOTSTRAP_S2C_KEY_OFFSET] == f["c2s_key"]
    assert bf[F.BOOTSTRAP_S2C_KEY_OFFSET:F.BOOTSTRAP_EXPIRE_AT_MS_OFFSET] == f["s2c_key"]
    assert int.from_bytes(bf[F.BOOTSTRAP_EXPIRE_AT_MS_OFFSET:F.BOOTSTRAP_FLAGS_OFFSET], "big") == f["expire_at_ms"]
    assert bf[F.BOOTSTRAP_FLAGS_OFFSET] == f["flags"]


def test_bootstrap_decode_roundtrip():
    bf = F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS)
    parsed = F.decode_bootstrap_frame(bf)
    assert parsed["connection_id"] == V.BOOTSTRAP_CONNECTION_ID
    assert parsed["session_token"] == V.BOOTSTRAP_SESSION_TOKEN
    assert parsed["c2s_key"] == V.BOOTSTRAP_C2S_KEY
    assert parsed["encryption_required"] is True


def test_bootstrap_outer_length_semantics():
    assert F.validate_bootstrap_outer_length(109)
    assert not F.validate_bootstrap_outer_length(81)


@pytest.mark.parametrize("mutant,reason", [
    (b"ORKX" + bytes.fromhex(V.VECTOR_A_HEX)[4:], "bad_magic"),
    (bytes.fromhex(V.VECTOR_A_HEX)[:4] + b"\x00\x01" + bytes.fromhex(V.VECTOR_A_HEX)[6:], "unsupported_version"),
    (bytes.fromhex(V.VECTOR_A_HEX)[:6] + b"\x02" + bytes.fromhex(V.VECTOR_A_HEX)[7:], "bad_type"),
    (bytes.fromhex(V.VECTOR_A_HEX)[:7] + b"\x01" + bytes.fromhex(V.VECTOR_A_HEX)[8:], "bad_reserved"),
    (bytes.fromhex(V.VECTOR_A_HEX)[:11] + b"\x60" + bytes.fromhex(V.VECTOR_A_HEX)[12:], "bad_payload_length"),
    (bytes.fromhex(V.VECTOR_A_HEX)[:108] + b"\x02", "bad_flags"),
])
def test_bootstrap_rejects_malformed(mutant, reason):
    with pytest.raises(ValueError) as exc:
        F.decode_bootstrap_frame(mutant)
    assert reason in str(exc.value)


@pytest.mark.parametrize("blob", [
    bytes.fromhex(V.VECTOR_A_HEX)[:81],
    bytes.fromhex(V.VECTOR_A_HEX)[:108],
    bytes.fromhex(V.VECTOR_A_HEX) + b"\x00",
])
def test_bootstrap_rejects_bad_size(blob):
    with pytest.raises(ValueError) as exc:
        F.decode_bootstrap_frame(blob)
    assert "bad_frame_size" in str(exc.value)


# ── Handshake V2（42B）──────────────────────────────────────────────────────


def test_handshake_key_derivation():
    assert F.derive_handshake_key(V.HANDSHAKE_C2S_KEY) == bytes.fromhex(V.HANDSHAKE_KEY_HEX)


def test_handshake_build_matches_known_vectors():
    p0 = F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 0, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    )
    p1 = F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 1, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    )
    assert len(p0) == F.UDP_HANDSHAKE_V2_SIZE == 42
    assert p0 == bytes.fromhex(V.HANDSHAKE_COUNTER0_HEX)
    assert p1 == bytes.fromhex(V.HANDSHAKE_COUNTER1_HEX)


def test_handshake_mac_input_layout():
    mac = F.build_handshake_mac_input(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_EPOCH, V.HANDSHAKE_NONCE, 0
    )
    assert mac == (b"ORK-UDP-HANDSHAKE-V2" + b"\x03\x02" + b"\x00\x00\x00\x0A"
                   + V.HANDSHAKE_EPOCH + V.HANDSHAKE_NONCE + b"\x00\x00\x00\x00")
    assert len(mac) == F.HANDSHAKE_MAC_INPUT_SIZE == 62


def test_handshake_verify_and_tamper():
    key = F.derive_handshake_key(V.HANDSHAKE_C2S_KEY)
    p0 = bytes.fromhex(V.HANDSHAKE_COUNTER0_HEX)
    assert F.verify_handshake_v2_auth_tag(p0, key, V.HANDSHAKE_EPOCH)
    tampered = bytearray(p0)
    tampered[10] ^= 0x01
    assert not F.verify_handshake_v2_auth_tag(bytes(tampered), key, V.HANDSHAKE_EPOCH)
    assert not F.verify_handshake_v2_auth_tag(p0, bytes(range(0xA0, 0xC0)), V.HANDSHAKE_EPOCH)


def test_handshake_decode_and_hard_reject():
    p0 = bytes.fromhex(V.HANDSHAKE_COUNTER0_HEX)
    frame = F.decode_handshake_v2_frame(p0)
    assert frame["connection_id"] == V.HANDSHAKE_CONNECTION_ID
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


def _read_golden(testdata_dir, name: str) -> bytes | None:
    p = testdata_dir / "golden" / name
    if not p.is_file():
        return None
    return bytes.fromhex("".join(p.read_text().split()))


def test_golden_bootstrap_matches(testdata_dir):
    g = _read_golden(testdata_dir, "bootstrap_v2.hex")
    if g is None:
        pytest.skip("golden/bootstrap_v2.hex 不存在")
    assert F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS) == g


def test_golden_handshake_matches(testdata_dir):
    g0 = _read_golden(testdata_dir, "handshake_v2_counter0.hex")
    g1 = _read_golden(testdata_dir, "handshake_v2_counter1.hex")
    if g0 is None or g1 is None:
        pytest.skip("golden/handshake_v2_*.hex 不存在")
    assert F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 0, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    ) == g0
    assert F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 1, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    ) == g1
