#!/usr/bin/env python3
"""Demo 02 — 可靠 (KCP) vs 不可靠 (UDP) 通道对比。

演示内容
--------
在 **同一条已建立的会话** 上，用相同的 25B `"ORKBENCH1"` 载荷分别经两个通道
各发送 N 次，测量并对比：

  - 送达率（delivery rate）：收到服务端原样回显的比例
  - 往返时延（RTT）：min / avg / p50 / p95 / p99 / max

两通道语义差异
  · Reliable(KCP)     —— ARQ 重传 + 有序，理论上送达率 ~100%，尾延迟略高
  · Unreliable(UDP)   —— 单发不重传，丢包即失序/丢失，延迟更低但送达率 <100%

为公平体现"不可靠"语义，本 demo 对每次发送 **单发单收、不重发**：某个 seq 在
超时窗口内未回显即计为一次丢失。（在 loopback 上通常无丢包，两通道都会接近
100%；接入真实弱网或用 netem 注入丢包后差异会显著放大。）

用法
----
    python demos/02_reliable_vs_unreliable/client.py
    python demos/02_reliable_vs_unreliable/client.py --iterations 200

退出码：0 = 两通道均至少有 1 次回显（demo 正常完成）；1 = 连接失败或全通道 0 送达。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lib.demo_helpers import (  # noqa: E402
    RELIABLE_MARKER,
    UNRELIABLE_MARKER,
    add_common_args,
    banner,
    connect_verbose,
    fail,
    format_rtt_table,
    info,
    marker_int,
    ok,
    rule,
    summarize_rtt,
    wait_echo,
)
from lib.ork_client_session import OrkClientSession  # noqa: E402

PER_ITER_TIMEOUT = 1.0  # 单次发送的回显等待窗口（秒），超时计为丢失


def run_channel(session: OrkClientSession, *, reliable: bool, base_seq: int, iterations: int) -> tuple[int, list[float]]:
    """在指定通道单发单收 iterations 次；返回 (送达数, RTT 采样列表 ms)。"""
    delivered = 0
    rtts: list[float] = []
    for i in range(iterations):
        seq = base_seq + i
        ts = time.perf_counter_ns()
        if reliable:
            session.send_bench_reliable(seq, ts)
        else:
            session.send_bench_unreliable(seq, ts)
        hit = wait_echo(session, reliable=reliable, want_seq=seq, sent_ts={ts}, timeout=PER_ITER_TIMEOUT)
        if hit is not None:
            delivered += 1
            rtts.append((time.perf_counter_ns() - hit) / 1e6)
    return delivered, rtts


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 02 — 可靠(KCP) vs 不可靠(UDP) 通道送达率与时延对比",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--iterations", "-n", type=int, default=100, help="每通道发送次数")
    args = parser.parse_args()

    if args.iterations < 1:
        print("--iterations 必须 >= 1", file=sys.stderr)
        return 1

    banner("DEMO 02 · Reliable vs Unreliable", f"目标 {args.host}:{args.port} · 每通道 {args.iterations} 次单发单收")

    session: OrkClientSession | None = None
    try:
        session = connect_verbose(args.host, args.port, "demo-02-chan")
        n = args.iterations

        rule(f"Reliable (KCP) · {n} 次")
        rel_base = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF  # 用低位区间避免与 UDP 段重叠
        t0 = time.perf_counter()
        rel_delivered, rel_rtts = run_channel(session, reliable=True, base_seq=rel_base + 1_000_000, iterations=n)
        rel_wall = time.perf_counter() - t0
        info(f"送达 {rel_delivered}/{n}  用时 {rel_wall:.2f}s")

        rule(f"Unreliable (UDP) · {n} 次")
        udp_base = marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF
        t0 = time.perf_counter()
        udp_delivered, udp_rtts = run_channel(session, reliable=False, base_seq=udp_base + 2_000_000, iterations=n)
        udp_wall = time.perf_counter() - t0
        info(f"送达 {udp_delivered}/{n}  用时 {udp_wall:.2f}s")
    except Exception as exc:  # noqa: BLE001
        fail(f"连接/收发失败: {exc}")
        return 1
    finally:
        if session is not None:
            session.close()

    rule("对比结果")
    rel_rate = 100.0 * rel_delivered / n
    udp_rate = 100.0 * udp_delivered / n
    print(f"  通道              送达率        送达数")
    print(f"  ───────────────  ──────────   ─────────")
    print(f"  Reliable (KCP)    {rel_rate:6.2f}%     {rel_delivered}/{n}")
    print(f"  Unreliable (UDP)  {udp_rate:6.2f}%     {udp_delivered}/{n}")
    print()
    print(format_rtt_table({
        "Reliable(KCP)": summarize_rtt(rel_rtts),
        "Unreliable(UDP)": summarize_rtt(udp_rtts),
    }))

    if rel_delivered == 0 and udp_delivered == 0:
        fail("两通道均 0 送达，demo 失败")
        return 1
    ok(f"对比完成：KCP 送达率 {rel_rate:.2f}% / UDP 送达率 {udp_rate:.2f}%")
    print("\n\033[32m✅ Demo 02 完成\033[0m")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
