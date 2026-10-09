# Demo 06 · 往返时延 (RTT) 测量与分位统计

在同一条会话上，对 **Reliable(KCP)** 与 **Unreliable(UDP)** 两通道各发送 N 个
25B `ORKBENCH1` ping，用 `time.perf_counter_ns()` 打点，测量每次回显的 RTT，
输出完整分位统计。

## 测量原理

- RTT = 从 `send` 前打点，到收到 `(seq, ts)` 完全匹配回显时的打点之差
- ping 载荷自带发送时间戳，服务端原样回显，故匹配即可精确计算往返耗时；
  全程单机 monotonic 时钟，**不依赖两端时钟同步**
- 为得到干净的时延分布，UDP 通道 **单发不重发**：丢失的 ping 跳过、不计入
  RTT 样本，但计入 `delivered/sent` 送达率
- 前 `--warmup` 次为预热（KCP 慢启动 / 缓存冷），不计入统计样本

## 输出

分位表：`count / min / avg / p50 / p95 / p99 / max`（单位 ms），以及两通道送达率。

## 运行

```bash
python demos/06_latency_measure/client.py
python demos/06_latency_measure/client.py --pings 500 --warmup 20
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--pings` / `-n` | 200 | 每通道 ping 次数 |
| `--warmup` | 10 | 预热次数（不计入统计） |

## 退出码

- `0` —— 两通道均采到有效 RTT 样本
- `1` —— 连接失败或无样本
