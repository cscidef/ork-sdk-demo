# Demo 01 · 最小 Echo

用最小代码走通 Ork 纯传输 SDK 的完整链路，并验证 **双通道 opaque 回显**。

## 展示内容

| 阶段 | 说明 |
|------|------|
| TCP Bootstrap | 连接后接收 **Bootstrap V2（109B opaque 帧）**：connectionId / sessionToken / c2sKey / s2cKey / bootstrapEpoch / 加密标志 |
| UDP + KCP | 绑定 UDP socket，创建 KCP 会话（`conv = connectionId`） |
| Handshake V2 | 发送 **42B 认证握手帧**（HKDF 派生 handshakeKey + HMAC-SHA256 authTag） |
| 等待 ACK | 收到服务端经 KCP 下发的 `"ORK-UDP-ACK:handshake"` → 连接就绪 |
| 双通道 Echo | Reliable(KCP) 与 Unreliable(UDP) 各发送 25B `ORKBENCH1` 帧，校验原样回显 |

回显帧格式：`b"ORKBENCH1"(9B) + marker(8B) + 发送时间戳 ns(8B LE) = 25B`
（服务端 `TransportDemoHandler` 的唯一 opaque 回显路径）。

## 运行

```bash
cd python && source .venv/bin/activate      # 若尚未 setup，先 bash setup.sh
bash ../server/start.sh                      # 启动 demo 服务端（端口 9400）
python demos/01_echo/client.py
python demos/01_echo/client.py --count 3     # 重复 3 轮
python demos/01_echo/client.py --host 127.0.0.1 --port 9400
```

## 退出码

- `0` —— 连接建立 + 双通道 echo 全部一致
- `1` —— 任一失败
