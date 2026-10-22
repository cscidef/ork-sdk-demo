#!/usr/bin/env python3
"""
Harmonia Ork 高压测试 Worker — 500 客户端 × 24h 持续负载。

架构:
  主进程 → fork shard_count 个子进程 (multiprocessing.Process)
  每子进程 → 管理 clients_per_worker 个 OrkClientSession (threading)
  每 stats_interval_sec → 写 JSONL checkpoint + TCP 推送 monitor_server

用法:
  python worker.py --config stress_config.yaml --node lnx140 --shard 0 --shard-count 5
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time

try:
    import resource  # Unix-only：Windows 上缺失时降级为 None
except ImportError:  # pragma: no cover - Windows
    resource = None
from collections import deque
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any

import yaml

# ---------------------------------------------------------------------------
# Path bootstrap: 确保可以 import OrkClientSession
# ---------------------------------------------------------------------------
_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from client.ork_client_session import (  # noqa: E402
    OrkClientSession,
    UnityPoseSimulator,
)
from metrics import WorkerReport, append_jsonl, percentile  # noqa: E402

# Machine IP for reporting (best-effort detection)
def _detect_machine_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"

_MACHINE_IP = _detect_machine_ip()

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
RELIABLE_MODES = ("interaction", "equipment", "chat")
MAX_RECONNECT_ATTEMPTS = 5
RTT_RING_SIZE = 1000


# ---------------------------------------------------------------------------
# Per-client statistics (thread-local counters, no lock needed for single writer)
# ---------------------------------------------------------------------------
@dataclass
class ClientStats:
    """每个客户端线程维护的计数器。"""

    poses_sent: int = 0
    reliable_sent: int = 0
    batch_sent: int = 0
    ack_sent: int = 0
    bytes_tx: int = 0
    errors_count: int = 0
    reconnects: int = 0
    rtt_samples: deque = field(default_factory=lambda: deque(maxlen=RTT_RING_SIZE))
    connected: bool = False


# ---------------------------------------------------------------------------
# Config loader
# ---------------------------------------------------------------------------
def load_config(config_path: str, node_name: str) -> dict[str, Any]:
    """读取 stress_config.yaml 并返回当前 node 的配置 dict。"""
    with open(config_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    server = cfg["server"]
    run = cfg["run"]
    workload = cfg["workload"]
    monitoring = cfg.get("monitoring", {})

    # 找到当前 node 配置
    node_cfg = None
    for n in cfg.get("nodes", []):
        if n["name"] == node_name:
            node_cfg = n
            break
    if node_cfg is None:
        raise ValueError(f"Node '{node_name}' not found in config nodes list")

    return {
        "host": server["host"],
        "port": int(server["port"]),
        "workers": int(node_cfg.get("workers", 1)),
        "clients_per_worker": int(node_cfg.get("clients_per_worker", 50)),
        "pose_hz": float(workload.get("pose_hz", 50)),
        "reliable_hz": float(workload.get("reliable_hz", 5)),
        "batch_hz": float(workload.get("batch_hz", 10)),
        "ack_hz": float(workload.get("ack_hz", 10)),
        "bone_count": int(workload.get("bone_count", 17)),
        "batch_entities": int(workload.get("batch_entities", 12)),
        "connect_stagger_ms": int(workload.get("connect_stagger_ms", 100)),
        "join_timeout_sec": float(workload.get("join_timeout_sec", 20)),
        "duration_sec": float(run.get("duration_hours", 24)) * 3600.0,
        "stats_interval_sec": float(run.get("stats_interval_sec", 60)),
        "run_id": str(run.get("id", "default")),
        "monitor_host": monitoring.get("host", "127.0.0.1"),
        "monitor_port": int(monitoring.get("port", 39000)),
    }


# ---------------------------------------------------------------------------
# Single client thread logic
# ---------------------------------------------------------------------------
def client_thread(
    index: int,
    cfg: dict[str, Any],
    stats: ClientStats,
    stop_event: threading.Event,
    uuid_prefix: str,
) -> None:
    """单个客户端线程：连接 → 主循环 → 断开。支持指数退避重连。"""
    host = cfg["host"]
    port = cfg["port"]
    uuid = f"{uuid_prefix}{index:05d}"
    pose_interval = 1.0 / cfg["pose_hz"] if cfg["pose_hz"] > 0 else 0
    reliable_interval = 1.0 / cfg["reliable_hz"] if cfg["reliable_hz"] > 0 else 0
    batch_interval = 1.0 / cfg["batch_hz"] if cfg["batch_hz"] > 0 else 0
    ack_interval = 1.0 / cfg["ack_hz"] if cfg["ack_hz"] > 0 else 0

    simulator = UnityPoseSimulator(
        seed=float(index) * 0.73,
        radius=6.0 + (index % 7),
        speed=1.5 + (index % 4) * 0.4,
    )
    device_meta = {
        "client": "stress-worker",
        "engine": "Unity2022.3",
        "platform": "StandaloneLinux64",
        "device_id": uuid,
    }

    reconnect_count = 0
    backoff = 1.0

    while not stop_event.is_set():
        session: OrkClientSession | None = None
        try:
            session = OrkClientSession(
                host, port, uuid,
                connect_timeout=10.0,
                join_timeout=cfg["join_timeout_sec"],
            )
            # --- Connection sequence ---
            session.connect_tcp_bootstrap()
            session.start_udp_rudp()

            # UDP handshake with retries
            for attempt in range(3):
                session.send_udp_handshake()
                try:
                    session.wait_udp_ack(timeout=5.0)
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
                    time.sleep(0.5 * (attempt + 1))

            session.send_join_and_wait(timeout=cfg["join_timeout_sec"])
            stats.connected = True
            reconnect_count = 0
            backoff = 1.0

            # --- Main loop ---
            sim_start = time.monotonic()
            next_pose = sim_start
            next_reliable = sim_start
            next_batch = sim_start
            next_ack = sim_start
            ack_step = 0
            reliable_seq = 0
            end_at = sim_start + cfg["duration_sec"]

            while time.monotonic() < end_at and not stop_event.is_set():
                now = time.monotonic()

                # Pump RUDP: receive server messages, measure RTT
                t_rtt_start = time.monotonic()
                msgs = session.pump_rudp()
                if msgs:
                    rtt_ms = (time.monotonic() - t_rtt_start) * 1000.0
                    stats.rtt_samples.append(rtt_ms)

                # Pose (UDP unreliable)
                if pose_interval > 0 and now >= next_pose:
                    t = now - sim_start
                    pos, rot, vel, ang_vel, anim_id = simulator.sample(t)
                    n = session.send_rich_pose(
                        pos, rot,
                        velocity=vel,
                        angular_velocity=ang_vel,
                        animation_state_id=anim_id,
                        bone_count=cfg["bone_count"],
                        device_metadata=device_meta,
                    )
                    stats.poses_sent += 1
                    stats.bytes_tx += n
                    next_pose = now + pose_interval

                # Reliable gameplay (RUDP)
                if reliable_interval > 0 and now >= next_reliable:
                    reliable_seq += 1
                    mode = RELIABLE_MODES[reliable_seq % len(RELIABLE_MODES)]
                    n = session.send_reliable_gameplay(
                        reliable_seq, mode,
                        interaction_target=2000 + index,
                    )
                    stats.reliable_sent += 1
                    stats.bytes_tx += n
                    next_reliable = now + reliable_interval

                # Entity batch (UDP unreliable)
                if batch_interval > 0 and now >= next_batch:
                    n = session.send_entity_batch(
                        cfg["batch_entities"],
                        base_entity_id=6000 + index * 100,
                    )
                    stats.batch_sent += 1
                    stats.bytes_tx += n
                    next_batch = now + batch_interval

                # Ack (RUDP)
                if ack_interval > 0 and now >= next_ack:
                    ack_step += 1
                    n = session.send_ack(ack_step)
                    stats.ack_sent += 1
                    stats.bytes_tx += n
                    next_ack = now + ack_interval

                # Yield GIL
                time.sleep(0.005)

            # Normal duration end
            break

        except Exception as exc:  # noqa: BLE001
            stats.errors_count += 1
            stats.connected = False
            if stop_event.is_set():
                break
            # Exponential backoff reconnect
            reconnect_count += 1
            stats.reconnects += 1
            if reconnect_count > MAX_RECONNECT_ATTEMPTS:
                break
            sleep_time = min(backoff, 16.0)
            backoff *= 2
            time.sleep(sleep_time)
        finally:
            if session is not None:
                try:
                    if stats.connected:
                        session.send_leave()
                except Exception:  # noqa: BLE001
                    pass
                try:
                    session.close()
                except Exception:  # noqa: BLE001
                    pass
                stats.connected = False

    stats.connected = False


# ---------------------------------------------------------------------------
# Stats reporter thread
# ---------------------------------------------------------------------------
def reporter_thread(
    cfg: dict[str, Any],
    node: str,
    shard: int,
    all_stats: list[ClientStats],
    stop_event: threading.Event,
    results_dir: Path,
    start_time: float,
) -> None:
    """周期性聚合统计 → JSONL + TCP push。"""
    interval = cfg["stats_interval_sec"]
    jsonl_path = str(results_dir / f"{node}_shard{shard}.jsonl")

    while not stop_event.wait(interval):
        report = _build_report(cfg, node, shard, all_stats, start_time)
        # Persist JSONL
        try:
            append_jsonl(jsonl_path, report)
        except OSError as e:
            print(f"[shard{shard}] JSONL write error: {e}", file=sys.stderr, flush=True)
        # TCP push to monitor
        _push_to_monitor(cfg, report)


def _build_report(
    cfg: dict[str, Any],
    node: str,
    shard: int,
    all_stats: list[ClientStats],
    start_time: float,
) -> WorkerReport:
    """聚合所有客户端线程的统计。"""
    now = time.time()

    total_poses = sum(s.poses_sent for s in all_stats)
    total_reliable = sum(s.reliable_sent for s in all_stats)
    total_batch = sum(s.batch_sent for s in all_stats)
    total_ack = sum(s.ack_sent for s in all_stats)
    total_bytes_tx = sum(s.bytes_tx for s in all_stats)
    total_errors = sum(s.errors_count for s in all_stats)
    total_reconnects = sum(s.reconnects for s in all_stats)
    joined = sum(1 for s in all_stats if s.connected)
    active = sum(1 for s in all_stats if s.connected)
    failed = len(all_stats) - joined

    # Aggregate RTT samples
    rtt_all: list[float] = []
    for s in all_stats:
        rtt_all.extend(s.rtt_samples)
    rtt_all.sort()
    avg_rtt = (sum(rtt_all) / len(rtt_all)) if rtt_all else 0.0
    p95_rtt = percentile(rtt_all, 95) if rtt_all else 0.0
    p99_rtt = percentile(rtt_all, 99) if rtt_all else 0.0

    # RSS (Linux/macOS: resource.getrusage; Windows 无 resource 模块 → 记 0)
    if resource is not None:
        usage = resource.getrusage(resource.RUSAGE_SELF)
        rss_mb = usage.ru_maxrss / 1024.0
    else:
        rss_mb = 0.0

    return WorkerReport(
        machine_ip=_MACHINE_IP,
        node_name=node,
        worker_id=shard,
        timestamp=now,
        clients_active=active,
        clients_joined=joined,
        clients_failed=failed,
        poses_sent=total_poses,
        reliable_sent=total_reliable,
        batch_sent=total_batch,
        ack_sent=total_ack,
        bytes_tx=total_bytes_tx,
        bytes_rx=0,
        avg_rtt_ms=round(avg_rtt, 3),
        p95_rtt_ms=round(p95_rtt, 3),
        p99_rtt_ms=round(p99_rtt, 3),
        errors_count=total_errors,
        reconnects=total_reconnects,
        memory_rss_mb=round(rss_mb, 2),
    )


def _push_to_monitor(cfg: dict[str, Any], report: WorkerReport) -> None:
    """TCP push JSON line to monitor_server (best-effort, no retry)."""
    host = cfg["monitor_host"]
    port = cfg["monitor_port"]
    try:
        sock = socket.create_connection((host, port), timeout=5.0)
        payload = json.dumps(asdict(report), separators=(",", ":")) + "\n"
        sock.sendall(payload.encode("utf-8"))
        sock.close()
    except OSError:
        pass  # monitor 不可达时静默


# ---------------------------------------------------------------------------
# Shard process entry
# ---------------------------------------------------------------------------
def run_shard(cfg: dict[str, Any], node: str, shard: int) -> None:
    """子进程入口：启动 N 个客户端线程 + 1 个 reporter 线程。"""
    clients_per_worker = cfg["clients_per_worker"]
    stagger_sec = cfg["connect_stagger_ms"] / 1000.0
    uuid_prefix = f"stress-{node}-s{shard}-"
    results_dir = _REPO_ROOT / "demo" / "stress" / "results" / cfg["run_id"]

    all_stats: list[ClientStats] = [ClientStats() for _ in range(clients_per_worker)]
    stop_event = threading.Event()
    start_time = time.time()

    threads: list[threading.Thread] = []

    # Launch client threads with stagger
    for i in range(clients_per_worker):
        if stop_event.is_set():
            break
        if i > 0 and stagger_sec > 0:
            time.sleep(stagger_sec)
        t = threading.Thread(
            target=client_thread,
            args=(i, cfg, all_stats[i], stop_event, uuid_prefix),
            daemon=True,
            name=f"client-{shard}-{i:04d}",
        )
        t.start()
        threads.append(t)

    # Launch reporter thread
    reporter = threading.Thread(
        target=reporter_thread,
        args=(cfg, node, shard, all_stats, stop_event, results_dir, start_time),
        daemon=True,
        name=f"reporter-{shard}",
    )
    reporter.start()

    print(
        f"[{node}/shard{shard}] Started {clients_per_worker} clients, "
        f"duration={cfg['duration_sec']/3600:.1f}h",
        flush=True,
    )

    # Wait for duration or interrupt
    try:
        while not stop_event.is_set():
            elapsed = time.time() - start_time
            if elapsed >= cfg["duration_sec"]:
                break
            # Check if all client threads died
            alive = sum(1 for t in threads if t.is_alive())
            if alive == 0:
                break
            stop_event.wait(5.0)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        for t in threads:
            t.join(timeout=3.0)
        print(f"[{node}/shard{shard}] Shutdown complete.", flush=True)


# ---------------------------------------------------------------------------
# Main: argument parsing + multiprocess fork
# ---------------------------------------------------------------------------
def main() -> int:
    import multiprocessing

    parser = argparse.ArgumentParser(
        description="Harmonia Ork stress test worker",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--config", required=True, help="Path to stress_config.yaml")
    parser.add_argument("--node", required=True, help="Current node name (matches yaml nodes[].name)")
    parser.add_argument("--shard", type=int, default=-1,
                        help="Run specific shard index (-1 = fork all shards via multiprocessing)")
    parser.add_argument("--shard-count", type=int, default=1, help="Total number of shards")
    args = parser.parse_args()

    cfg = load_config(args.config, args.node)
    shard_count = args.shard_count

    # Single-process mode (shard_count=1 or explicit --shard)
    if shard_count <= 1:
        run_shard(cfg, args.node, 0)
        return 0

    if args.shard >= 0:
        # Explicit shard: run only this one (external coordinator manages others)
        if args.shard >= shard_count:
            print(f"Error: --shard {args.shard} >= --shard-count {shard_count}", file=sys.stderr)
            return 1
        run_shard(cfg, args.node, args.shard)
        return 0

    # Fork mode: main process spawns shard_count children
    print(
        f"[{args.node}] Forking {shard_count} shard processes "
        f"({cfg['clients_per_worker']} clients each = "
        f"{shard_count * cfg['clients_per_worker']} total)",
        flush=True,
    )
    children: list[multiprocessing.Process] = []
    for s in range(shard_count):
        p = multiprocessing.Process(
            target=run_shard,
            args=(cfg, args.node, s),
            name=f"{args.node}-shard{s}",
            daemon=False,
        )
        p.start()
        children.append(p)

    # Wait for all children (handle SIGINT gracefully)
    try:
        for p in children:
            p.join()
    except KeyboardInterrupt:
        print(f"\n[{args.node}] Interrupted, terminating shards...", flush=True)
        for p in children:
            if p.is_alive():
                p.terminate()
        for p in children:
            p.join(timeout=5.0)
        return 130

    exit_codes = [p.exitcode or 0 for p in children]
    if any(c != 0 for c in exit_codes):
        print(f"[{args.node}] Some shards exited with errors: {exit_codes}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
