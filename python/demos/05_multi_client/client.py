#!/usr/bin/env python3
"""Demo 05 — 多客户端并发（N 个会话同时 echo，聚合成功率）。

演示内容
--------
并发拉起 N 个独立 OrkClientSession（每个走完整 bootstrap→handshake→connected，
拥有各自的 connectionId、UDP socket、KCP 会话），每个客户端做若干次 Reliable(KCP)
echo，最后聚合统计：

  · 连接成功数 / 失败数
  · echo 总成功率（送达回显数 / 发送数）
  · 每客户端 RTT 概览 + 全局聚合 min/avg/max

并发实现：每客户端一个线程（Python 线程足以驱动各自的 socket + KCP pump，
KCP bridge 默认支持 256 个并发会话，见 lib/kcp/kcp_bridge.c）。connectionId
由服务端为每条 TCP bootstrap 分配，天然隔离，无需客户端协调。

用法
----
    python demos/05_multi_client/client.py
    python demos/05_multi_client/client.py --clients 10 --echoes 5

退出码：0 = 全部客户端连接成功且 echo 成功率 100%；1 = 有任何失败。
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lib.demo_helpers import (  # noqa: E402
    RELIABLE_MARKER,
    add_common_args,
    banner,
    echo_once_reliable,
    fail,
    info,
    marker_int,
    ok,
    rule,
    summarize_rtt,
)
from lib.ork_client_session import OrkClientSession  # noqa: E402


@dataclass
class ClientResult:
    index: int
    connected: bool = False
    conn_id: int = 0
    echoes_sent: int = 0
    echoes_ok: int = 0
    rtts: list[float] = field(default_factory=list)
    error: str = ""


def run_client(host: str, port: int, index: int, echoes: int, results: list[ClientResult]) -> None:
    """单个客户端线程体：连接 + echoes 次 Reliable echo。"""
    res = ClientResult(index=index)
    session: OrkClientSession | None = None
    try:
        session = OrkClientSession(host, port, f"demo-05-client#{index}")
        session.connect()  # bootstrap + handshake + wait ACK（静默）
        res.connected = True
        res.conn_id = session.connection_id
        base = (marker_int(RELIABLE_MARKER) & 0xFFFF) * 1000 + index * 100_000
        for k in range(echoes):
            res.echoes_sent += 1
            rtt = echo_once_reliable(session, base + k)
            if rtt is not None:
                res.echoes_ok += 1
                res.rtts.append(rtt)
    except Exception as exc:  # noqa: BLE001
        res.error = str(exc)
    finally:
        if session is not None:
            session.close()
        results[index] = res


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 05 — N 个客户端并发 echo，聚合成功率",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--clients", "-c", type=int, default=5, help="并发客户端数量")
    parser.add_argument("--echoes", "-e", type=int, default=3, help="每个客户端的 Reliable echo 次数")
    args = parser.parse_args()

    if args.clients < 1 or args.echoes < 1:
        print("--clients 与 --echoes 必须 >= 1", file=sys.stderr)
        return 1

    banner("DEMO 05 · Multi-Client", f"目标 {args.host}:{args.port} · 并发客户端 {args.clients} · 每客户端 echo {args.echoes}")

    results: list[ClientResult] = [ClientResult(index=i) for i in range(args.clients)]
    threads: list[threading.Thread] = []

    rule("并发启动")
    t0 = time.perf_counter()
    for i in range(args.clients):
        th = threading.Thread(target=run_client, args=(args.host, args.port, i, args.echoes, results), daemon=True)
        threads.append(th)
        th.start()
    for th in threads:
        th.join(timeout=60)
    wall = time.perf_counter() - t0
    info(f"{args.clients} 个客户端并发执行完毕，用时 {wall:.2f}s")

    rule("每客户端结果")
    print("    #   connected   connId     echo(ok/sent)   avgRTT(ms)")
    print("    ──  ─────────   ───────    ─────────────   ──────────")
    all_rtts: list[float] = []
    connected = 0
    total_sent = total_ok = 0
    for r in results:
        connected += 1 if r.connected else 0
        total_sent += r.echoes_sent
        total_ok += r.echoes_ok
        all_rtts.extend(r.rtts)
        avg = (sum(r.rtts) / len(r.rtts)) if r.rtts else 0.0
        flag = "✓" if (r.connected and r.echoes_ok == r.echoes_sent and r.echoes_sent > 0) else "✗"
        err = f"  err={r.error}" if r.error else ""
        print(f"    {r.index:<2}  {flag} {str(r.connected):<9}  {r.conn_id:<8}  {r.echoes_ok:>3}/{r.echoes_sent:<3}          {avg:>7.2f}{err}")

    rule("聚合")
    conn_rate = 100.0 * connected / args.clients
    echo_rate = 100.0 * total_ok / total_sent if total_sent else 0.0
    print(f"  连接成功:   {connected}/{args.clients}  ({conn_rate:.1f}%)")
    print(f"  echo 送达:  {total_ok}/{total_sent}  ({echo_rate:.1f}%)")
    if all_rtts:
        st = summarize_rtt(all_rtts)
        print(f"  全局 RTT:   min={st['min']:.2f}  avg={st['avg']:.2f}  p95={st['p95']:.2f}  max={st['max']:.2f} (ms)")

    success = connected == args.clients and total_ok == total_sent and total_sent > 0
    if success:
        ok("全部客户端连接成功且 echo 100% 送达")
        print("\n\033[32m✅ Demo 05 完成\033[0m")
        return 0
    fail("存在失败客户端或未送达 echo")
    print("\n\033[31m❌ Demo 05 存在失败\033[0m")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
