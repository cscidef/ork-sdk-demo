# Demo 03 · 传输密码层（ChaCha20-Poly1305-IETF AEAD）

剖析 Ork 传输密码层的帧结构与加解密，并在真实会话上验证端到端加密 echo。

## 展示内容

**Part A — 离线（确定性，不连服务器）**

- 加密前后帧结构：`15B OrkFrame 帧头 + AEAD 密文 + 16B Poly1305 tag`
- 15B 帧头逐字段拆解：`ver(1) + seq(8 LE) + payload_len(2 LE) + packet_type(4 LE)`
- nonce(12B) / AAD 的构造（域分隔，绑定 direction·channel·seq·conn·token·type·len）
- `seal → open` 往返一致
- **重放检测**：同一帧二次 open 被拒绝
- **完整性**：篡改密文一字节 → open 返回 None

**Part B — 在线**

- 报告本次会话协商出的加密模式（Bootstrap flags → `encryption_required`）
- 双通道发送 echo，每个回显都经 AEAD 解密 + 完整性校验，统计成功次数

## 运行

```bash
python demos/03_encryption/client.py --offline-only   # 只跑 Part A，无需服务端
python demos/03_encryption/client.py                  # Part A + Part B
python demos/03_encryption/client.py --rounds 5
```

## 退出码

- `0` —— 通过
- `1` —— 失败
