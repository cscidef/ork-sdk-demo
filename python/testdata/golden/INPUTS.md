# Golden 向量输入说明（冻结）

本目录 hex 由 `testdata/generate.py` 生成，输入参数固定于 `tests/vectors.py`，
改动即破坏与 C++ 参考实现的逐字节对齐。

## bootstrap_v2.hex（109B）

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

## handshake_v2_counter0.hex / _counter1.hex（42B each）

`ork_frame.build_handshake_v2_packet(conn=10, nonce, counter, epoch, c2s_key)`：

- c2s_key = bytes(range(0x20,0x40))
- bootstrap_epoch = bytes(range(16))
- client_nonce = bytes(range(0x10,0x20)) = 10 11 .. 1F
- connection_id = 10
- counter = 0 / 1

## cipher_seal.hex（37B）

`ork_cipher.seal(state, CHANNEL_RUDP, C2S, packet_type=5, plaintext)`：

- c2s_key = 0x11 * 32
- s2c_key = 0x22 * 32
- connection_id = 0x12345678
- session_token = 0xAABBCCDD
- encryption_required = True
- plaintext = DE AD BE EF 01 02
- 首帧 tx_seq = 1（seq 从 1 起）
- 帧结构：15B 帧头 + AEAD 密文(6B) + Poly1305 tag(16B) = 37B
