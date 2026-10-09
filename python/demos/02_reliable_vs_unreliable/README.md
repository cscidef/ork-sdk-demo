# Demo 02 · 可靠 (KCP) vs 不可靠 (UDP)

在 **同一条会话** 上，用相同的 25B `ORKBENCH1` 载荷分别经两个通道各发送 N 次，
对比 **送达率** 与 **往返时延分布**。

## 两通道语义

| 通道 | 机制 | 预期 |
|------|------|------|
| Reliable (KCP) | ARQ 重传 + 有序交付 | 送达率 ~100%，尾延迟略高 |
| Unreliable (UDP) | 单发不重传 | 延迟更低，弱网下送达率 <100% |

为体现"不可靠"语义，每次发送 **单发单收、不重发**：某 seq 在超时窗口内未回显即
计为一次丢失。

> loopback 上通常无丢包，两通道都会接近 100%。接入真实弱网、或用 `tc/netem`
> 注入丢包后，KCP 与 UDP 的送达率差异会显著放大。

## 运行

```bash
python demos/02_reliable_vs_unreliable/client.py
python demos/02_reliable_vs_unreliable/client.py --iterations 200
```

输出：两通道送达率对比表 + RTT 分位表（min/avg/p50/p95/p99/max）。

## 退出码

- `0` —— demo 正常完成（两通道均至少 1 次回显）
- `1` —— 连接失败或两通道 0 送达
