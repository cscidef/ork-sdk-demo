# Demo 05 · 多客户端并发

并发拉起 **N 个独立会话**（默认 5），每个都走完整的
bootstrap → handshake → connected 流程，拥有各自的 connectionId、UDP socket
与 KCP 会话，各做若干次 Reliable(KCP) echo，最后聚合统计成功率与时延。

## 展示内容

- 每客户端独立连接：connectionId 由服务端为每条 TCP bootstrap 分配，天然隔离，
  客户端之间无需协调
- 并发模型：每客户端一个线程（各自驱动 socket + KCP pump）；KCP bridge 默认
  支持 256 个并发会话（见 `lib/kcp/kcp_bridge.c`）
- 聚合指标：
  - 连接成功数 / 失败数
  - echo 总送达率（收到回显数 / 发送数）
  - 每客户端 avgRTT + 全局 RTT（min / avg / p95 / max）

## 运行

```bash
python demos/05_multi_client/client.py
python demos/05_multi_client/client.py --clients 10 --echoes 5
python demos/05_multi_client/client.py -c 8 -e 3
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--clients` / `-c` | 5 | 并发客户端数量 |
| `--echoes` / `-e` | 3 | 每个客户端的 Reliable echo 次数 |

## 退出码

- `0` —— 全部客户端连接成功且 echo 100% 送达
- `1` —— 存在失败客户端或未送达 echo
