# Demo 04 · 连接生命周期

显式跟踪并打印一条会话的完整状态迁移：
**connect → send → idle → disconnect → reconnect**。

## 状态迁移

```
DISCONNECTED
  → BOOTSTRAP      TCP 连接 + 收 Bootstrap V2 (109B)
  → HANDSHAKING    UDP bind + KCP + Handshake V2 (42B)
  → CONNECTED      收到 "ORK-UDP-ACK:"
  → SENDING        双通道 echo
  → IDLE           静默若干秒，验证连接保持（idle 后再次 echo 证明存活）
  → DISCONNECTING  close(): 停收包线程 + 释放 KCP + 关 socket
  → DISCONNECTED
  → (RECONNECT)    重新走一遍，得到新的 connectionId
```

## 观察重点

- 断开后再连接会得到 **新的 connectionId**（服务端为每条 TCP bootstrap 分配）
- IDLE 期间不发包，连接仍存活（服务端 idle timeout 默认 300s）
- `close()` 是幂等的资源回收（KCP 会话 + UDP/TCP socket + 后台线程）

## 运行

```bash
python demos/04_connection_lifecycle/client.py
python demos/04_connection_lifecycle/client.py --idle 3 --reconnects 2
```

## 退出码

- `0` —— 全部生命周期阶段成功
- `1` —— 任一阶段失败
