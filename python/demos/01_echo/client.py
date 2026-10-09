#!/usr/bin/env python3
"""Demo 01 — 最小 echo（连接建立 + 双通道回显）。

演示内容
--------
用最小的代码走通 Ork 纯传输 SDK 的完整链路，并验证双通道回显：

  1. TCP 连接，接收 **Bootstrap V2（109B opaque 帧）**：拿到 connectionId /
     sessionToken / c2sKey / s2cKey / bootstrapEpoch / 加密标志。
  2. 绑定 UDP socket + 创建 KCP 会话（conv = connectionId）。
  3. 发送 **UDP Handshake V2（42B 认证帧）**：由 c2sKey 经 HKDF 派生 handshakeKey，
     HMAC-SHA256 生成 authTag（覆盖除自身外全部明文字段）。
  4. 等待服务端经 KCP 可靠通道下发的 opaque 文本 `"ORK-UDP-ACK:handshake"`。
  5. 在 **Reliable(KCP)** 与 **Unreliable(UDP)** 两通道各发送一个 25B
     `"ORKBENCH1"` echo 帧，校验服务端原样回显。

回显帧格式（服务端 TransportDemoHandler 唯一 opaque 回显路径）：
    b"ORKBENCH1"(9B) + marker(8B) + 发送时间戳 ns(8B LE) = 25B
  - Reliable 通道 marker = b"ORKDEMO1"
  - Unreliable 通道 marker = b"ORKDEMO2"

用法
----
    python demos/01_echo/client.py
    python demos/01_echo/client.py --host 127.0.0.1 --port 9400 --count 3

退出码：0 = 全部通过；1 = 有失败。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# 让脚本无论从哪里运行都能 import 到 python/lib
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lib.demo_helpers import (  # noqa: E402
    ECHO_FRAME_SIZE,
    RELIABLE_MARKER,
    RELIABLE_ECHO_TIMEOUT,
    UNRELIABLE_MARKER,
    UDP_ECHO_ATTEMPTS,
    UDP_ECHO_TIMEOUT,
    add_common_args,
    banner,
    connect_verbose,
    fail,
    info,
    marker_int,
    ok,
    rule,
    wait_echo,
)
from lib.ork_client_session import OrkClientSession  # noqa: E402


def round_reliable(session: OrkClientSession) -> bool:
    seq = marker_int(RELIABLE_MARKER)
    ts = time.perf_counter_ns()
    session.send_bench_reliable(seq, ts)
    info(f"[Reliable/KCP]   发送 {ECHO_FRAME_SIZE}B: ORKBENCH1 + {RELIABLE_MARKER.decode()} + ts={ts}")
    hit = wait_echo(session, reliable=True, want_seq=seq, sent_ts={ts}, timeout=RELIABLE_ECHO_TIMEOUT)
    if hit is None:
        fail(f"[Reliable/KCP]   {RELIABLE_ECHO_TIMEOUT:.0f}s 内未收到回显")
        return False
    rtt = (time.perf_counter_ns() - hit) / 1e6
    ok(f"[Reliable/KCP]   回显一致  RTT={rtt:.2f}ms")
    return True


def round_unreliable(session: OrkClientSession) -> bool:
    seq = marker_int(UNRELIABLE_MARKER)
    sent: set[int] = set()
    for attempt in range(1, UDP_ECHO_ATTEMPTS + 1):
        ts = time.perf_counter_ns()
        sent.add(ts)
        wire = session.send_bench_unreliable(seq, ts)
        label = "发送" if attempt == 1 else f"重发 {attempt}/{UDP_ECHO_ATTEMPTS}"
        info(f"[Unreliable/UDP] {label} {ECHO_FRAME_SIZE}B (wire {wire}B): ORKBENCH1 + {UNRELIABLE_MARKER.decode()}")
        hit = wait_echo(session, reliable=False, want_seq=seq, sent_ts=sent, timeout=UDP_ECHO_TIMEOUT)
        if hit is not None:
            rtt = (time.perf_counter_ns() - hit) / 1e6
            ok(f"[Unreliable/UDP] 回显一致  RTT={rtt:.2f}ms")
            return True
    fail(f"[Unreliable/UDP] {UDP_ECHO_ATTEMPTS} 次尝试均未收到回显（不可靠通道）")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 01 — 最小 echo：连接建立 + 双通道 opaque 回显验证",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--count", type=int, default=1, help="echo 重复轮次")
    args = parser.parse_args()

    if args.count < 1:
        print("--count 必须 >= 1", file=sys.stderr)
        return 1

    banner("DEMO 01 · 最小 Echo", f"目标 {args.host}:{args.port} · 轮次 {args.count} · 载荷 {ECHO_FRAME_SIZE}B opaque（零 Proto）")

    session: OrkClientSession | None = None
    ok_rel = ok_unrel = 0
    try:
        session = connect_verbose(args.host, args.port, "demo-01-echo")
        for i in range(1, args.count + 1):
            rule(f"Echo 轮次 {i}/{args.count}")
            if round_reliable(session):
                ok_rel += 1
            if round_unreliable(session):
                ok_unrel += 1
    except Exception as exc:  # noqa: BLE001
        fail(f"连接/收发失败: {exc}")
        return 1
    finally:
        if session is not None:
            session.close()

    rule("汇总")
    print(f"  连接建立:          ✓ TCP bootstrap + UDP handshake + KCP ACK")
    print(f"  Reliable (KCP):    {ok_rel}/{args.count} 回显一致 ({RELIABLE_MARKER.decode()})")
    print(f"  Unreliable (UDP):  {ok_unrel}/{args.count} 回显一致 ({UNRELIABLE_MARKER.decode()})")

    if ok_rel == args.count and ok_unrel == args.count:
        print("\n\033[32m✅ 全部通过：连接建立 + 双通道 opaque echo 验证成功\033[0m")
        return 0
    print("\n\033[31m❌ 存在失败轮次\033[0m")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
