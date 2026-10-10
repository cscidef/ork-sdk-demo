"""Canonical test vectors for Ork protocol validation.

单一来源（single source of truth）：本模块集中定义所有离线单元测试与 golden
向量生成所需的输入参数与已知答案 hex。使用者：

  - testdata/generate.py     冻结 golden/*.hex 与 payloads/*.bin
  - tests/test_frame_codec.py  Bootstrap V2 / Handshake V2 编解码向量校验
  - tests/test_cipher.py       ChaCha20-Poly1305 seal/open 向量校验

这些取值与 C++ bootstrap_v2_test.cpp（§14.2 向量组 A）、
ork_handshake_v2_test.cpp（向量组 B）、ork_cipher_test.cpp 逐字节对齐；改动即
破坏跨语言一致性，务必同步 C++ 侧。
"""

from __future__ import annotations

# ── Bootstrap V2（109B）输入 ────────────────────────────────────────────────
BOOTSTRAP_CONNECTION_ID = 10
BOOTSTRAP_SESSION_TOKEN = 0xDEADBEEF
BOOTSTRAP_EPOCH = bytes(range(16))            # 00 01 .. 0F
BOOTSTRAP_C2S_KEY = bytes(range(0x20, 0x40))  # 20 21 .. 3F
BOOTSTRAP_S2C_KEY = bytes(range(0x40, 0x60))  # 40 41 .. 5F
BOOTSTRAP_EXPIRE_AT_MS = 1790000000000
BOOTSTRAP_FLAGS = 0x01                        # encryption_required

# encode_bootstrap_frame(**BOOTSTRAP_INPUTS) 的完整入参
BOOTSTRAP_INPUTS = dict(
    connection_id=BOOTSTRAP_CONNECTION_ID,
    session_token=BOOTSTRAP_SESSION_TOKEN,
    bootstrap_epoch=BOOTSTRAP_EPOCH,
    c2s_key=BOOTSTRAP_C2S_KEY,
    s2c_key=BOOTSTRAP_S2C_KEY,
    expire_at_ms=BOOTSTRAP_EXPIRE_AT_MS,
    flags=BOOTSTRAP_FLAGS,
)

# 向量组 A：109B Bootstrap V2 帧的已知答案 hex
VECTOR_A_HEX = (
    "4F524B4200020100000000610000000ADEADBEEF000102030405060708090A0B0C0D0E0F"
    "202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F"
    "404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F"
    "000001A0C4506C0001"
)

# ── Handshake V2（42B）输入 ─────────────────────────────────────────────────
# 沿用向量组 A 的 c2sKey 与 bootstrapEpoch；clientNonce 取递增 16B（0x10–0x1F）。
HANDSHAKE_CONNECTION_ID = BOOTSTRAP_CONNECTION_ID
HANDSHAKE_C2S_KEY = BOOTSTRAP_C2S_KEY
HANDSHAKE_EPOCH = BOOTSTRAP_EPOCH
HANDSHAKE_NONCE = bytes(range(0x10, 0x20))    # 10 11 .. 1F

# HKDF-SHA-256(c2sKey) 派生的 32B handshakeKey（§14.2 冻结）
HANDSHAKE_KEY_HEX = "4A7B7076F11E316A6D3B3F5D716AEE01D7E6FC536A05A562877F250D2E3DC4DB"

# 向量组 B：42B Handshake V2 帧的已知答案 hex（counter = 0 / 1）
HANDSHAKE_COUNTER0_HEX = (
    "03020000000A101112131415161718191A1B1C1D1E1F00000000177B22F12707695BBA6212D69EB54033"
)
HANDSHAKE_COUNTER1_HEX = (
    "03020000000A101112131415161718191A1B1C1D1E1F00000001D3D3019CAB1E5A77A52F030AA39B112F"
)

# ── Cipher（ChaCha20-Poly1305-IETF AEAD）输入 ───────────────────────────────
CIPHER_C2S_KEY = bytes([0x11] * 32)
CIPHER_S2C_KEY = bytes([0x22] * 32)
CIPHER_CONNECTION_ID = 0x12345678
CIPHER_SESSION_TOKEN = 0xAABBCCDD
CIPHER_ENCRYPTION_REQUIRED = True
CIPHER_PLAIN = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02])

# CipherState(**CIPHER_STATE) 的完整入参
CIPHER_STATE = dict(
    c2s_key=CIPHER_C2S_KEY,
    s2c_key=CIPHER_S2C_KEY,
    connection_id=CIPHER_CONNECTION_ID,
    session_token=CIPHER_SESSION_TOKEN,
    encryption_required=CIPHER_ENCRYPTION_REQUIRED,
)

# ── payload 确定性模式（testdata/payloads）──────────────────────────────────
PAYLOAD_SIZES = [1, 64, 512, 1200]


def payload_pattern(n: int) -> bytes:
    """第 i 字节 = (i * 31 + 7) & 0xFF 的可预测模式，用于确定性测试输入。"""
    return bytes((i * 31 + 7) & 0xFF for i in range(n))
