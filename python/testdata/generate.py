#!/usr/bin/env python3
"""生成 testdata/：payloads/（各尺寸可预测模式）+ golden/（冻结 hex 向量）。

用法（在 python/ 目录下，需已安装 PyNaCl）::

    python testdata/generate.py

产物
----
payloads/
    payload_1.bin / payload_64.bin / payload_512.bin / payload_1200.bin
        第 i 字节 = (i * 31 + 7) & 0xFF 的可预测模式，用于确定性测试输入。
golden/
    bootstrap_v2.hex          —— 109B Bootstrap V2 帧（向量组 A）
    handshake_v2_counter0.hex —— 42B Handshake V2（counter=0，向量组 B）
    handshake_v2_counter1.hex —— 42B Handshake V2（counter=1，重试）
    cipher_seal.hex           —— 已知密钥/明文下 seal() 的确定性 AEAD 输出
    INPUTS.md                 —— 冻结上述向量所用的精确输入参数

所有输入参数统一来自 tests/vectors.py（单一来源），golden 向量与
tests/test_frame_codec.py / test_cipher.py 及 C++ bootstrap_v2_test.cpp /
ork_handshake_v2_test.cpp / ork_cipher_test.cpp 逐字节对齐。
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent          # .../testdata
PYTHON_DIR = HERE.parent                         # .../python
TESTS_DIR = PYTHON_DIR / "tests"                 # .../python/tests（vectors.py 所在）
for _p in (PYTHON_DIR, TESTS_DIR):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import vectors as V                   # noqa: E402
from lib import ork_cipher as C       # noqa: E402
from lib import ork_frame as F        # noqa: E402


def gen_payloads(payloads_dir: Path) -> None:
    payloads_dir.mkdir(parents=True, exist_ok=True)
    for n in V.PAYLOAD_SIZES:
        p = payloads_dir / f"payload_{n}.bin"
        p.write_bytes(V.payload_pattern(n))
        print(f"  ✓ {p.relative_to(HERE)}  ({n}B)")


def _write_hex(golden_dir: Path, name: str, data: bytes) -> None:
    (golden_dir / name).write_text(data.hex() + "\n")
    print(f"  ✓ golden/{name}  ({len(data)}B)")


def gen_golden(golden_dir: Path) -> None:
    golden_dir.mkdir(parents=True, exist_ok=True)

    bootstrap = F.encode_bootstrap_frame(**V.BOOTSTRAP_INPUTS)
    assert len(bootstrap) == F.BOOTSTRAP_FRAME_SIZE
    _write_hex(golden_dir, "bootstrap_v2.hex", bootstrap)

    hs0 = F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 0, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    )
    hs1 = F.build_handshake_v2_packet(
        V.HANDSHAKE_CONNECTION_ID, V.HANDSHAKE_NONCE, 1, V.HANDSHAKE_EPOCH, V.HANDSHAKE_C2S_KEY
    )
    assert len(hs0) == len(hs1) == F.UDP_HANDSHAKE_V2_SIZE
    _write_hex(golden_dir, "handshake_v2_counter0.hex", hs0)
    _write_hex(golden_dir, "handshake_v2_counter1.hex", hs1)

    st = C.CipherState(**V.CIPHER_STATE)
    sealed = C.seal(st, C.CHANNEL_RUDP, C.C2S, 5, V.CIPHER_PLAIN)
    assert sealed is not None
    _write_hex(golden_dir, "cipher_seal.hex", sealed)

    _write_inputs_doc(golden_dir, bootstrap, hs0, sealed)


def _write_inputs_doc(golden_dir: Path, bootstrap: bytes, hs0: bytes, sealed: bytes) -> None:
    doc = f"""# Golden 向量输入说明（冻结）

本目录 hex 由 `testdata/generate.py` 生成，输入参数固定于 `tests/vectors.py`，
改动即破坏与 C++ 参考实现的逐字节对齐。

## bootstrap_v2.hex（{len(bootstrap)}B）

`ork_frame.encode_bootstrap_frame(...)`：

| 字段 | 值 |
|------|-----|
| connection_id | 10 |
| session_token | 0xDEADBEEF |
| bootstrap_epoch | bytes(range(16)) = 00 01 .. 0F |
| c2s_key | bytes(range(0x20,0x40)) = 20 21 .. 3F |
| s2c_key | bytes(range(0x40,0x60)) = 40 41 .. 5F |
| expire_at_ms | 1790000000000 |
| flags | 0x01 (encryption_required) |

## handshake_v2_counter0.hex / _counter1.hex（{len(hs0)}B each）

`ork_frame.build_handshake_v2_packet(conn=10, nonce, counter, epoch, c2s_key)`：

- c2s_key = bytes(range(0x20,0x40))
- bootstrap_epoch = bytes(range(16))
- client_nonce = bytes(range(0x10,0x20)) = 10 11 .. 1F
- connection_id = 10
- counter = 0 / 1

## cipher_seal.hex（{len(sealed)}B）

`ork_cipher.seal(state, CHANNEL_RUDP, C2S, packet_type=5, plaintext)`：

- c2s_key = 0x11 * 32
- s2c_key = 0x22 * 32
- connection_id = 0x12345678
- session_token = 0xAABBCCDD
- encryption_required = True
- plaintext = DE AD BE EF 01 02
- 首帧 tx_seq = 1（seq 从 1 起）
- 帧结构：15B 帧头 + AEAD 密文(6B) + Poly1305 tag(16B) = {len(sealed)}B
"""
    (golden_dir / "INPUTS.md").write_text(doc)
    print("  ✓ golden/INPUTS.md")


def main() -> int:
    print("=== 生成 testdata ===")
    print("[payloads]")
    gen_payloads(HERE / "payloads")
    print("[golden]")
    gen_golden(HERE / "golden")
    print("完成。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
