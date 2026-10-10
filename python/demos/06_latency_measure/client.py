#!/usr/bin/env python3
"""Demo 06 — 往返时延 (RTT) 测量与分位统计。

演示内容
--------
在同一条会话上，对 Reliable(KCP) 与 Unreliable(UDP) 两通道各发送 N 个
25B `"ORKBENCH1"` ping，用 `time.perf_counter_ns()` 打点，测量每次回显的 RTT，
输出分位统计：

    count / min / avg / p50 / p95 / p99 / max

RTT 定义：从 send 前的 perf_counter_ns 打点，到收到 (seq, ts) 完全匹配回显时的
perf_counter_ns 之差。ping 载荷本身携带发送时间戳，服务端原样回显，故匹配即
可精确计算往返耗时（不依赖时钟同步，全程单机 monotonic 时钟）。

为得到干净的时延分布，UDP 通道单发不重发（丢失的 ping 直接跳过，不计入 RTT
样本，但计入 delivered/sent 送达率）。

用法
----
    python demos/06_latency_measure/client.py
    python demos/06_latency_measure/client.py --pings 500

退出码：0 = 两通道均采到有效 RTT 样本；1 = 连接失败或无样本。
"""

from __future__ import annotations

import argparse
import sys
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
    run_channel,
    summarize_rtt,
)
from lib.ork_client_session import OrkClientSession  # noqa: E402

PING_TIMEOUT = 2.0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 06 — 双通道 RTT 测量与分位统计 (min/avg/p50/p95/p99/max)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--pings", "-n", type=int, default=200, help="每通道 ping 次数")
    parser.add_argument("--warmup", type=int, default=10, help="预热次数（不计入统计）")
    args = parser.parse_args()

    if args.pings < 1:
        print("--pings 必须 >= 1", file=sys.stderr)
        return 1
    warmup = min(args.warmup, max(0, args.pings - 1))

    banner("DEMO 06 · Latency Measure", f"目标 {args.host}:{args.port} · 每通道 {args.pings} ping（预热 {warmup}）")

    session: OrkClientSession | None = None
    try:
        session = connect_verbose(args.host, args.port, "demo-06-latency")

        rule(f"Reliable (KCP) · {args.pings} pings")
        rel = run_channel(
            session, reliable=True,
            base_seq=(marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF) + 3_000_000,
            iterations=args.pings, warmup=warmup, per_iter_timeout=PING_TIMEOUT,
        )
        info(f"送达 {rel.delivered}/{args.pings} · 有效样本 {len(rel.rtts)}")

        rule(f"Unreliable (UDP) · {args.pings} pings")
        udp = run_channel(
            session, reliable=False,
            base_seq=(marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF) + 4_000_000,
            iterations=args.pings, warmup=warmup, per_iter_timeout=PING_TIMEOUT,
        )
        info(f"送达 {udp.delivered}/{args.pings} · 有效样本 {len(udp.rtts)}")
    except Exception as exc:  # noqa: BLE001
        fail(f"连接/测量失败: {exc}")
        return 1
    finally:
        if session is not None:
            session.close()

    rule("RTT 统计 (ms)")
    print(format_rtt_table({
        "Reliable(KCP)": summarize_rtt(rel.rtts),
        "Unreliable(UDP)": summarize_rtt(udp.rtts),
    }))
    print()
    print(f"  送达率:  Reliable {100.0 * rel.delivered / args.pings:.2f}%   ·   Unreliable {100.0 * udp.delivered / args.pings:.2f}%")

    if not rel.rtts and not udp.rtts:
        fail("两通道均无有效 RTT 样本")
        return 1
    ok("RTT 测量完成")
    print("\n\033[32m✅ Demo 06 完成\033[0m")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
