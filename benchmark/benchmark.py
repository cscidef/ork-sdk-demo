#!/usr/bin/env python3
"""Unified benchmark client for reference transports and the real Ork demo."""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import statistics
import struct
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

# Phase 6.2: 应用层弱网损伤注入。impairment 只依赖标准库，模块级导入安全。
from client.impairment import ImpairmentEngine, get_profile  # noqa: E402


@dataclass
class ClientResult:
    connected: bool = False
    connect_ms: float = 0.0
    reliable_sent: int = 0
    reliable_received: int = 0
    realtime_sent: int = 0
    realtime_received: int = 0
    bytes_sent: int = 0
    reliable_rtt_ms: list[float] = field(default_factory=list)
    realtime_rtt_ms: list[float] = field(default_factory=list)
    # Phase 6.3: Unreliable(UDP) 通道乱序计数（收到的 seq 未严格递增的次数）
    realtime_reorder: int = 0
    handshake_retries: int = 0
    error: str = ""


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * quantile) - 1))
    return ordered[index]


def frame(sequence: int, size: int) -> bytes:
    header = struct.pack("!Qd", sequence, time.monotonic())
    return header + bytes(max(0, size - len(header)))


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("peer closed")
        chunks.extend(chunk)
    return bytes(chunks)


def tcp_roundtrip(sock: socket.socket, payload: bytes,
                  engine: "ImpairmentEngine | None" = None) -> float:
    wire = struct.pack("!I", len(payload)) + payload
    start = time.perf_counter()
    # Phase 6.2: TCP 是可靠流，应用层丢包无意义（内核会重传），只注入 delay/限速。
    # start 在 delay 之前采样，让 RTT 把单向延迟计进去（与 netem 语义对齐）。
    if engine is not None:
        engine.apply_delay(len(wire))
    sock.sendall(wire)
    size = struct.unpack("!I", recv_exact(sock, 4))[0]
    recv_exact(sock, size)
    return (time.perf_counter() - start) * 1000.0


def run_reference_client(args: argparse.Namespace, index: int, result: ClientResult) -> None:
    # Phase 6.2: 每客户端独立 engine（独立 RNG + reorder buffer，避免锁竞争）。
    # unshaped 档 is_passthrough() 为真，engine 会短路，零开销。
    engine = ImpairmentEngine(get_profile(args.network),
                              seed=args.impairment_seed + index)
    tcp: socket.socket | None = None
    udp: socket.socket | None = None
    try:
        started = time.perf_counter()
        if args.transport in ("tcp_only", "tcp_udp"):
            tcp = socket.create_connection((args.host, args.port), timeout=args.timeout)
            tcp.settimeout(args.timeout)
        if args.transport in ("udp_only", "tcp_udp"):
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.connect((args.host, args.port))
            udp.settimeout(args.timeout)
        result.connected = True
        result.connect_ms = (time.perf_counter() - started) * 1000.0
        end_at = time.monotonic() + args.duration
        next_reliable = next_realtime = time.monotonic()
        reliable_sequence = realtime_sequence = 0
        while time.monotonic() < end_at:
            now = time.monotonic()
            if args.reliable_hz > 0 and now >= next_reliable:
                reliable_sequence += 1
                payload = frame(reliable_sequence, args.payload_bytes)
                if args.transport == "udp_only":
                    assert udp is not None
                    before = time.perf_counter()
                    # UDP 不可靠：全损伤（delay + loss + dup）。丢包时 send 不上线，
                    # 下面的 recv 超时，自然体现为送达率下降。
                    engine.impair_send(payload, udp.send)
                    result.reliable_sent += 1
                    result.bytes_sent += len(payload)
                    try:
                        udp.recv(65535)
                        result.reliable_received += 1
                        result.reliable_rtt_ms.append((time.perf_counter() - before) * 1000.0)
                    except socket.timeout:
                        pass
                else:
                    assert tcp is not None
                    result.reliable_sent += 1
                    result.bytes_sent += len(payload) + 4
                    result.reliable_rtt_ms.append(tcp_roundtrip(tcp, payload, engine))
                    result.reliable_received += 1
                next_reliable += 1.0 / args.reliable_hz
            if args.pose_hz > 0 and now >= next_realtime:
                realtime_sequence += 1
                payload = frame(realtime_sequence, args.payload_bytes)
                if args.transport == "tcp_only":
                    assert tcp is not None
                    result.realtime_sent += 1
                    result.bytes_sent += len(payload) + 4
                    result.realtime_rtt_ms.append(tcp_roundtrip(tcp, payload, engine))
                    result.realtime_received += 1
                else:
                    assert udp is not None
                    before = time.perf_counter()
                    engine.impair_send(payload, udp.send)
                    result.realtime_sent += 1
                    result.bytes_sent += len(payload)
                    try:
                        udp.recv(65535)
                        result.realtime_received += 1
                        result.realtime_rtt_ms.append((time.perf_counter() - before) * 1000.0)
                    except socket.timeout:
                        pass
                next_realtime += 1.0 / args.pose_hz
            time.sleep(0.0005)
    except Exception as exc:  # noqa: BLE001
        result.error = str(exc)
    finally:
        # Phase 6.2: flush 损伤引擎延迟队列（socket 仍开）再关 socket，并 join 后台线程
        engine.stop()
        if tcp is not None:
            tcp.close()
        if udp is not None:
            udp.close()


def run_ork_client(args: argparse.Namespace, index: int, result: ClientResult) -> None:
    from client.ork_client_session import OrkClientSession

    # Phase 6.2: 每客户端独立 engine，注入到 session 的线缆层收发路径。
    engine = ImpairmentEngine(get_profile(args.network),
                              seed=args.impairment_seed + index)
    session = OrkClientSession(args.host, args.port, f"bench-{os.getpid()}-{index}",
                               connect_timeout=args.timeout, join_timeout=args.timeout,
                               impairment=engine)
    try:
        started = time.perf_counter()
        session.connect_tcp_bootstrap()
        session.start_udp_rudp()
        handshake_ok = False
        for attempt in range(args.handshake_attempts):
            session.send_udp_handshake()
            if session._wait_for_udp_ack(timeout=args.handshake_timeout):  # benchmark recovery probe
                handshake_ok = True
                result.handshake_retries = attempt
                break
        if not handshake_ok:
            raise TimeoutError(
                f"未收到 UdpAck (attempts={args.handshake_attempts}, "
                f"timeout={args.handshake_timeout}s)"
            )
        session.send_join_and_wait(timeout=args.timeout)
        result.connected = True
        result.connect_ms = (time.perf_counter() - started) * 1000.0

        # Phase 4.0: ORKBENCH1 双通道 ping 测量
        # Phase 6.3: Unreliable(UDP) 通道独立指标 —— RTT 百分位 / 送达率 / 乱序率
        end_at = time.monotonic() + args.duration
        next_reliable = next_realtime = time.monotonic()
        reliable_seq = realtime_seq = 0
        prev_udp_seq: int | None = None

        def _ingest_rudp(seq: int, ts_ns: int) -> None:
            rtt_ms = (time.perf_counter_ns() - ts_ns) / 1_000_000.0
            result.reliable_received += 1
            result.reliable_rtt_ms.append(rtt_ms)

        def _ingest_udp(seq: int, ts_ns: int) -> None:
            nonlocal prev_udp_seq
            rtt_ms = (time.perf_counter_ns() - ts_ns) / 1_000_000.0
            result.realtime_received += 1
            result.realtime_rtt_ms.append(rtt_ms)
            # 乱序：本次到达的 seq 相对上一个到达的 seq 未严格递增
            if prev_udp_seq is not None and seq <= prev_udp_seq:
                result.realtime_reorder += 1
            prev_udp_seq = seq

        while time.monotonic() < end_at:
            now = time.monotonic()
            busy = False
            # 泵收 RUDP → 触发 ORKBENCH1 回显检测
            session.pump_rudp()
            for seq, ts_ns in session.drain_bench_rudp():
                _ingest_rudp(seq, ts_ns)
                busy = True
            for seq, ts_ns in session.drain_bench_udp():
                _ingest_udp(seq, ts_ns)
                busy = True
            # Reliable 通道 ORKBENCH1 ping
            if args.reliable_hz > 0 and now >= next_reliable:
                reliable_seq += 1
                send_ns = time.perf_counter_ns()
                result.bytes_sent += session.send_bench_reliable(reliable_seq, send_ns)
                result.reliable_sent += 1
                next_reliable += 1.0 / args.reliable_hz
                busy = True
            # Unreliable 通道 ORKBENCH1 ping
            if args.pose_hz > 0 and now >= next_realtime:
                realtime_seq += 1
                send_ns = time.perf_counter_ns()
                result.bytes_sent += session.send_bench_unreliable(realtime_seq, send_ns)
                result.realtime_sent += 1
                next_realtime += 1.0 / args.pose_hz
                busy = True
            # Phase 6.3: 移除主循环固定 1ms sleep（会给 RTT 注入 ≤1ms 抖动、拉高 P50）。
            # 改为仅在本轮无任何收发时短暂让出 CPU；有数据时立即再处理，兼顾精度与 CPU。
            if not busy:
                time.sleep(0.0002)
        # 排空期：等待尾包
        deadline = time.monotonic() + min(2.0, args.timeout)
        while time.monotonic() < deadline:
            busy = False
            session.pump_rudp()
            for seq, ts_ns in session.drain_bench_rudp():
                _ingest_rudp(seq, ts_ns)
                busy = True
            for seq, ts_ns in session.drain_bench_udp():
                _ingest_udp(seq, ts_ns)
                busy = True
            if result.reliable_received >= result.reliable_sent:
                break
            if not busy:
                time.sleep(0.002)
        session.send_leave()
    except Exception as exc:  # noqa: BLE001
        result.error = str(exc)
    finally:
        session.close()


def summarize(args: argparse.Namespace, results: list[ClientResult], wall_seconds: float) -> dict:
    reliable_rtt = [value for item in results for value in item.reliable_rtt_ms]
    realtime_rtt = [value for item in results for value in item.realtime_rtt_ms]
    connected = sum(item.connected for item in results)
    reliable_sent = sum(item.reliable_sent for item in results)
    reliable_received = sum(item.reliable_received for item in results)
    realtime_sent = sum(item.realtime_sent for item in results)
    realtime_received = sum(item.realtime_received for item in results)
    realtime_reorder = sum(item.realtime_reorder for item in results)

    # Phase 6.3: UDP(Unreliable) 通道独立指标。realtime 通道仅在非 tcp_only
    # 时才真正走 UDP；tcp_only 的 realtime 走 TCP，故 udp_* 置 None（报告显示“—”）。
    realtime_is_udp = args.transport in ("udp_only", "tcp_udp", "ork_hybrid")
    udp_rtt_p50 = percentile(realtime_rtt, 0.50) if realtime_is_udp else None
    udp_rtt_p95 = percentile(realtime_rtt, 0.95) if realtime_is_udp else None
    udp_rtt_p99 = percentile(realtime_rtt, 0.99) if realtime_is_udp else None
    udp_delivery_rate = (
        (realtime_received / realtime_sent)
        if (realtime_is_udp and realtime_sent) else None
    )
    udp_loss_rate = (1.0 - udp_delivery_rate) if udp_delivery_rate is not None else None
    udp_reorder_rate = (
        (realtime_reorder / realtime_received)
        if (realtime_is_udp and realtime_received) else None
    )
    return {
        "schema_version": 1,
        "transport": args.transport,
        "network": args.network,
        "clients": args.clients,
        "duration_s": args.duration,
        "wall_s": wall_seconds,
        "pose_hz": args.pose_hz,
        "reliable_hz": args.reliable_hz,
        "payload_bytes": args.payload_bytes,
        "bone_count": args.bone_count if args.transport == "ork_hybrid" else None,
        "connected": connected,
        "connect_success_pct": connected * 100.0 / args.clients,
        "connect_ms_avg": statistics.fmean([r.connect_ms for r in results if r.connected]) if connected else None,
        "connect_ms_p95": percentile([r.connect_ms for r in results if r.connected], 0.95),
        "reliable_sent": reliable_sent,
        "reliable_received": reliable_received,
        "reliable_delivery_pct": reliable_received * 100.0 / reliable_sent if reliable_sent else None,
        "reliable_rtt_ms_p50": percentile(reliable_rtt, 0.50),
        "reliable_rtt_ms_p95": percentile(reliable_rtt, 0.95),
        "reliable_rtt_ms_p99": percentile(reliable_rtt, 0.99),
        # Phase 6.3: RUDP 命名空间别名（与 udp_* 对称，便于门禁比较）
        "rudp_rtt_p50": percentile(reliable_rtt, 0.50),
        "rudp_rtt_p95": percentile(reliable_rtt, 0.95),
        "rudp_rtt_p99": percentile(reliable_rtt, 0.99),
        "realtime_sent": realtime_sent,
        "realtime_received": realtime_received,
        "realtime_delivery_pct": (realtime_received * 100.0 / realtime_sent)
        if realtime_sent else None,
        "realtime_rtt_ms_p95": percentile(realtime_rtt, 0.95),
        # Phase 6.3: UDP(Unreliable) 通道实测指标
        "udp_rtt_p50": udp_rtt_p50,
        "udp_rtt_p95": udp_rtt_p95,
        "udp_rtt_p99": udp_rtt_p99,
        "udp_delivery_rate": udp_delivery_rate,
        "udp_loss_rate": udp_loss_rate,
        "udp_reorder_rate": udp_reorder_rate,
        "udp_reorder_count": realtime_reorder,
        "bytes_sent": sum(item.bytes_sent for item in results),
        "offered_mbps": sum(item.bytes_sent for item in results) * 8.0 / max(args.duration, 0.001) / 1_000_000,
        "errors": [item.error for item in results if item.error],
        "handshake_retries": sum(item.handshake_retries for item in results),
        "raw_clients": [asdict(item) for item in results],
    }


def main() -> int:
    parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--transport", required=True,
                        choices=("tcp_only", "udp_only", "tcp_udp", "ork_hybrid"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--clients", type=int, default=5)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--quick", action="store_true",
                        help="快速档：clients=2, duration=3（对齐 run_benchmark.sh --quick 单次采集规模；"
                             "会覆盖 --clients/--duration）")
    parser.add_argument("--pose-hz", type=float, default=30.0)
    parser.add_argument("--reliable-hz", type=float, default=5.0)
    parser.add_argument("--payload-bytes", type=int, default=256)
    parser.add_argument("--bone-count", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--handshake-attempts", type=int, default=3)
    parser.add_argument("--handshake-timeout", type=float, default=2.0)
    parser.add_argument("--connect-stagger-ms", type=float, default=50.0)
    parser.add_argument("--network", default="unshaped",
                        help="unshaped 不注入；broadband/mobile/weak 启用应用层损伤（Phase 6.2）")
    parser.add_argument("--impairment-seed", type=int, default=20260917,
                        help="损伤引擎 RNG 基种子；每客户端用 seed+index，保证可复现")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # Phase 6.2: --quick 便捷档（让 `benchmark.py --network mobile --quick` 可直接跑）。
    if args.quick:
        args.clients = 2
        args.duration = 3.0
    if args.clients < 1 or args.duration <= 0 or args.payload_bytes < 16:
        parser.error("clients>=1, duration>0, payload-bytes>=16 are required")
    # Phase 6.2: 提前校验 --network，未知档位走 argparse 报错而不是静默降级。
    try:
        get_profile(args.network)
    except ValueError as exc:
        parser.error(str(exc))

    results = [ClientResult() for _ in range(args.clients)]
    threads: list[threading.Thread] = []
    started = time.monotonic()
    for index, result in enumerate(results):
        target = run_ork_client if args.transport == "ork_hybrid" else run_reference_client
        # 两条路径现在签名统一为 (args, index, result)
        thread = threading.Thread(target=target, args=(args, index, result), daemon=True)
        thread.start()
        threads.append(thread)
        time.sleep(args.connect_stagger_ms / 1000.0)
    for thread in threads:
        thread.join(args.duration + args.timeout * 3)

    summary = summarize(args, results, time.monotonic() - started)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items() if key not in ("raw_clients", "errors")},
                     ensure_ascii=False, indent=2))
    return 0 if summary["connected"] > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
