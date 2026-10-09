#!/usr/bin/env python3
"""Demo 04 — 连接生命周期（connect → send → idle → disconnect → reconnect）。

演示内容
--------
显式跟踪并打印一条会话的状态迁移，覆盖完整生命周期：

    DISCONNECTED
      → BOOTSTRAP      (TCP 连接 + 收 Bootstrap V2 109B)
      → HANDSHAKING    (UDP bind + KCP + Handshake V2 42B)
      → CONNECTED      (收到 "ORK-UDP-ACK:")
      → SENDING        (双通道 echo)
      → IDLE           (静默若干秒，验证连接保持)
      → DISCONNECTING  (close：停收包线程 + 释放 KCP + 关 socket)
      → DISCONNECTED
      → (RECONNECT)    (重新走一遍 bootstrap→handshake→connected，新 connectionId)

重点观察：
  · 断开后再连接会得到 **新的 connectionId**（服务端为每条 TCP bootstrap 分配）；
  · IDLE 期间不发包，连接仍存活（服务端 idle timeout 默认 300s）；
  · close() 是幂等的资源回收（KCP 会话 + UDP/TCP socket + 后台线程）。

用法
----
    python demos/04_connection_lifecycle/client.py
    python demos/04_connection_lifecycle/client.py --idle 3 --reconnects 2

退出码：0 = 全部生命周期阶段成功；1 = 任一阶段失败。
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
    echo_once_reliable,
    echo_once_unreliable,
    fail,
    info,
    marker_int,
    ok,
    rule,
)
from lib.ork_client_session import OrkClientSession  # noqa: E402


class Lifecycle:
    """极简状态机，仅用于把生命周期阶段可视化打印。"""

    def __init__(self) -> None:
        self.state = "DISCONNECTED"
        self.history: list[str] = [self.state]

    def to(self, state: str, note: str = "") -> None:
        prev = self.state
        self.state = state
        self.history.append(state)
        suffix = f"  ({note})" if note else ""
        print(f"  {prev:>13}  ──▶  \033[36m{state}\033[0m{suffix}")


def one_lifecycle(host: str, port: int, idx: int, idle_sec: float) -> tuple[bool, int]:
    """跑一轮完整生命周期；返回 (是否成功, connectionId)。"""
    lc = Lifecycle()
    session = OrkClientSession(host, port, f"demo-04-life#{idx}")
    conn_id = 0
    try:
        rule(f"生命周期 #{idx}")
        lc.to("BOOTSTRAP", "TCP + Bootstrap V2 109B")
        summary = session.connect_tcp_bootstrap()
        conn_id = session.connection_id
        ok(f"{summary}")

        lc.to("HANDSHAKING", "UDP + KCP + Handshake V2 42B")
        session.start_udp_rudp()
        session.send_udp_handshake()

        lc.to("CONNECTED", "等待 ORK-UDP-ACK:")
        ack = session.wait_udp_ack()
        ok(f"{ack}")

        lc.to("SENDING", "双通道 echo")
        r = echo_once_reliable(session, marker_int(RELIABLE_MARKER) + idx)
        u = echo_once_unreliable(session, marker_int(UNRELIABLE_MARKER) + idx, attempts=3)
        if r is None:
            fail("Reliable echo 未回显")
            return False, conn_id
        ok(f"Reliable ✓ RTT={r:.2f}ms · Unreliable {'✓' if u is not None else '✗(不可靠通道可容忍)'}")

        lc.to("IDLE", f"静默 {idle_sec:.1f}s 验证连接保持")
        time.sleep(idle_sec)
        # idle 后仍可通信 → 再打一次 reliable echo 证明连接存活
        r2 = echo_once_reliable(session, marker_int(RELIABLE_MARKER) + idx + 1000)
        if r2 is None:
            fail("IDLE 后 Reliable echo 失败（连接可能已断）")
            return False, conn_id
        ok(f"IDLE 后仍连通 ✓ RTT={r2:.2f}ms")

        lc.to("DISCONNECTING", "close(): 停线程 + 释放 KCP + 关 socket")
        session.close()
        lc.to("DISCONNECTED")
        ok(f"生命周期 #{idx} 完成 (conn={conn_id})")
        return True, conn_id
    except Exception as exc:  # noqa: BLE001
        fail(f"生命周期 #{idx} 失败: {exc}")
        try:
            session.close()
        except Exception:  # noqa: BLE001
            pass
        return False, conn_id


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 04 — 连接生命周期：connect→send→idle→disconnect→reconnect",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--idle", type=float, default=2.0, help="IDLE 阶段静默秒数")
    parser.add_argument("--reconnects", type=int, default=1, help="额外重连次数（总轮次 = 1 + reconnects）")
    args = parser.parse_args()

    total = 1 + max(0, args.reconnects)
    banner("DEMO 04 · Connection Lifecycle", f"目标 {args.host}:{args.port} · 生命周期轮次 {total}（含重连）")

    conn_ids: list[int] = []
    all_ok = True
    for idx in range(1, total + 1):
        label = "初次连接" if idx == 1 else f"重连 #{idx - 1}"
        info(f"—— {label} ——")
        passed, cid = one_lifecycle(args.host, args.port, idx, args.idle)
        all_ok = all_ok and passed
        conn_ids.append(cid)
        if idx < total:
            time.sleep(0.3)  # 重连间隔

    rule("汇总")
    print(f"  生命周期轮次: {total}")
    print(f"  connectionId 序列: {conn_ids}")
    uniq = len(set(c for c in conn_ids if c))
    if uniq > 1:
        ok(f"重连分配了不同的 connectionId（{uniq} 个唯一值），符合预期")
    elif total > 1:
        info("提示：本次未观察到 connectionId 变化（服务端可能复用了分配）")

    if all_ok:
        print("\n\033[32m✅ Demo 04 完成：全部生命周期阶段成功\033[0m")
        return 0
    print("\n\033[31m❌ Demo 04 存在失败阶段\033[0m")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
