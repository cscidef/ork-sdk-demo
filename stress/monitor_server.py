#!/usr/bin/env python3
"""Harmonia Ork 压力测试监控聚合服务。

运行在服务端机器上，监听 TCP 端口（默认 39000，由 stress_config.yaml monitoring.port 配置）
接收各 worker 推送的统计数据，定时采集服务端指标并刷新终端仪表盘。

用法:
    python monitor_server.py --config stress_config.yaml --run-id <id> [--ork-pid <pid>]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socketserver
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any

import yaml

# 确保能 import 同目录下的 lib
sys.path.insert(0, str(Path(__file__).resolve().parent))
from metrics import WorkerReport, ServerStats, append_jsonl, read_jsonl  # noqa: E402

# ─── 全局状态 ────────────────────────────────────────────────────────────────

_workers: dict[str, dict[str, Any]] = {}  # key: "ip:worker_id"
_workers_lock = threading.Lock()
_server_stats: dict[str, Any] = {}
_stats_lock = threading.Lock()
_start_time: float = 0.0
_run_id: str = ""
_results_dir: Path = Path(".")
_ork_pid: int | None = None
_shutdown = threading.Event()


# ─── TCP Handler ─────────────────────────────────────────────────────────────

class WorkerReportHandler(socketserver.StreamRequestHandler):
    """接收 Worker 推送的一行 JSON 报告。"""

    def handle(self) -> None:
        try:
            line = self.rfile.readline(65536)
            if not line:
                return
            data = json.loads(line.decode("utf-8").strip())
            key = f"{data.get('machine_ip', '?')}:{data.get('worker_id', '?')}"
            with _workers_lock:
                _workers[key] = data
                _workers[key]["_received_at"] = time.time()
            # 落盘
            append_jsonl(_results_dir / "timeseries.jsonl", data)
        except Exception as exc:
            print(f"[WARN] WorkerReportHandler error: {exc}", file=sys.stderr)


# ─── 服务端指标采集 (每 60s) ─────────────────────────────────────────────────

def _collect_ork_process_info(pid: int) -> dict[str, Any]:
    """读取 ork 服务端进程 CPU/RSS。"""
    try:
        out = subprocess.check_output(
            ["ps", "-o", "pid,rss,pcpu", "-p", str(pid)],
            text=True, timeout=5
        )
        lines = out.strip().split("\n")
        if len(lines) >= 2:
            parts = lines[1].split()
            return {"pid": int(parts[0]), "rss_kb": int(parts[1]), "cpu_pct": float(parts[2])}
    except Exception:
        pass
    return {"pid": pid, "rss_kb": 0, "cpu_pct": 0.0}


def _detect_ork_pid() -> int | None:
    """自动检测 ork 服务端 (embed_minimal) 进程 PID。"""
    try:
        out = subprocess.check_output(["pgrep", "-f", "embed_minimal"], text=True, timeout=5)
        pids = [int(p) for p in out.strip().split("\n") if p.strip()]
        return pids[0] if pids else None
    except Exception:
        return None


def _parse_ork_log_aggregate(log_path: Path) -> dict[str, Any]:
    """解析 ork.log 中 TransportDemoHandler 最新聚合行。"""
    result: dict[str, Any] = {}
    try:
        with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if "transportConnectionCount" in line:
                    m = re.search(r"transportConnectionCount=(\d+)", line)
                    if m:
                        result["transport_connections"] = int(m.group(1))
                    m = re.search(r"sessionConnectedCount=(\d+)", line)
                    if m:
                        result["session_connected"] = int(m.group(1))
                    m = re.search(r"joinedCount=(\d+)", line)
                    if m:
                        result["joined_count"] = int(m.group(1))
    except Exception:
        pass
    return result


def _collect_netstat() -> dict[str, int]:
    """macOS 网卡统计 (en0)。"""
    try:
        out = subprocess.check_output(["netstat", "-I", "en0", "-b"], text=True, timeout=5)
        lines = out.strip().split("\n")
        if len(lines) >= 2:
            parts = lines[1].split()
            # Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll
            return {"ibytes": int(parts[6]), "obytes": int(parts[9])}
    except Exception:
        pass
    return {"ibytes": 0, "obytes": 0}


def _server_stats_loop(interval: int = 60) -> None:
    """定时采集服务端指标。"""
    global _ork_pid
    project_root = Path(__file__).resolve().parent.parent.parent
    log_path = project_root / "logs" / "ork.log"

    if _ork_pid is None:
        _ork_pid = _detect_ork_pid()

    prev_net = _collect_netstat()
    prev_time = time.time()

    while not _shutdown.is_set():
        _shutdown.wait(interval)
        if _shutdown.is_set():
            break

        now = time.time()
        elapsed = now - prev_time
        proc_info = _collect_ork_process_info(_ork_pid) if _ork_pid else {}
        log_info = _parse_ork_log_aggregate(log_path)
        cur_net = _collect_netstat()

        # 计算网络速率
        tx_mbps = (cur_net.get("obytes", 0) - prev_net.get("obytes", 0)) * 8 / elapsed / 1e6 if elapsed > 0 else 0
        rx_mbps = (cur_net.get("ibytes", 0) - prev_net.get("ibytes", 0)) * 8 / elapsed / 1e6 if elapsed > 0 else 0

        stats = {
            "timestamp": now,
            "ork_pid": _ork_pid,
            "cpu_pct": proc_info.get("cpu_pct", 0.0),
            "rss_mb": proc_info.get("rss_kb", 0) / 1024.0,
            "transport_connections": log_info.get("transport_connections", 0),
            "session_connected": log_info.get("session_connected", 0),
            "joined_count": log_info.get("joined_count", 0),
            "net_tx_mbps": round(tx_mbps, 2),
            "net_rx_mbps": round(rx_mbps, 2),
        }
        with _stats_lock:
            global _server_stats
            _server_stats = stats

        append_jsonl(_results_dir / "server_stats.jsonl", stats)
        prev_net = cur_net
        prev_time = now


# ─── 终端仪表盘 (每 5s) ──────────────────────────────────────────────────────

_STALE_THRESHOLD = 300  # 5 分钟无推送视为 stale


def _render_dashboard() -> str:
    """生成仪表盘文本。"""
    now = time.time()
    elapsed = timedelta(seconds=int(now - _start_time))
    time_str = str(elapsed).split(".")[0]

    with _workers_lock:
        workers = list(_workers.values())
    with _stats_lock:
        stats = dict(_server_stats)

    total_clients = sum(w.get("clients_active", 0) for w in workers)
    joined = sum(w.get("clients_joined", 0) for w in workers)
    failed = sum(w.get("clients_failed", 0) for w in workers)
    reconnects = sum(w.get("reconnects", 0) for w in workers)
    errors = sum(w.get("errors_count", 0) for w in workers)

    # 计算每秒吞吐（取最新一批的累计值除以运行时长）
    run_sec = max(now - _start_time, 1)
    poses = sum(w.get("poses_sent", 0) for w in workers)
    reliable = sum(w.get("reliable_sent", 0) for w in workers)
    batch = sum(w.get("batch_sent", 0) for w in workers)
    acks = sum(w.get("ack_sent", 0) for w in workers)

    # RTT 取所有 worker 的加权均值
    rtts_p50 = [w.get("avg_rtt_ms") for w in workers if w.get("avg_rtt_ms")]
    rtts_p95 = [w.get("p95_rtt_ms") for w in workers if w.get("p95_rtt_ms")]
    rtts_p99 = [w.get("p99_rtt_ms") for w in workers if w.get("p99_rtt_ms")]
    p50 = statistics.fmean(rtts_p50) if rtts_p50 else 0
    p95 = statistics.fmean(rtts_p95) if rtts_p95 else 0
    p99 = statistics.fmean(rtts_p99) if rtts_p99 else 0

    # 带宽（从服务端指标）
    tx_mbps = stats.get("net_tx_mbps", 0)
    rx_mbps = stats.get("net_rx_mbps", 0)

    # stale 检测
    stale_keys = []
    with _workers_lock:
        for k, w in _workers.items():
            if now - w.get("_received_at", 0) > _STALE_THRESHOLD:
                stale_keys.append(k)

    last_update = datetime.now().strftime("%H:%M:%S")
    sep = "═" * 71

    lines = [
        sep,
        f"  Harmonia Ork 压力测试监控  |  Run: {_run_id}  |  已运行: {time_str}",
        sep,
        f"  客户端: {joined}/{total_clients} 已连接  |  失败: {failed}  |  重连: {reconnects}",
        f"  吞吐: Pose {poses/run_sec:,.0f}/s | Reliable {reliable/run_sec:,.0f}/s | Batch {batch/run_sec:,.0f}/s | Ack {acks/run_sec:,.0f}/s",
        f"  带宽: ↑ {tx_mbps:.1f} Mbps  ↓ {rx_mbps:.1f} Mbps",
        f"  RTT:  P50={p50:.1f}ms  P95={p95:.1f}ms  P99={p99:.1f}ms",
        f"  服务端: CPU {stats.get('cpu_pct', 0):.0f}% | RSS {stats.get('rss_mb', 0):.0f}MB | Joined {stats.get('joined_count', 0)} | Connections {stats.get('transport_connections', 0)}",
        f"  错误: {errors}  |  最后更新: {last_update}",
    ]
    if stale_keys:
        lines.append(f"  ⚠️  Stale workers: {', '.join(stale_keys[:5])}")
    lines.append(sep)
    return "\n".join(lines)


def _dashboard_loop(interval: int = 5) -> None:
    """定时刷新终端仪表盘。"""
    # 清屏一次
    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()
    while not _shutdown.is_set():
        frame = _render_dashboard()
        # 光标移到顶部，清除到末尾
        sys.stdout.write(f"\033[H\033[J{frame}\n")
        sys.stdout.flush()
        _shutdown.wait(interval)


# ─── 入口 ────────────────────────────────────────────────────────────────────

def _load_config(path: Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def main() -> int:
    global _start_time, _run_id, _results_dir, _ork_pid

    parser = argparse.ArgumentParser(description="Harmonia Ork 压力测试监控聚合服务")
    parser.add_argument("--config", type=Path, required=True, help="stress_config.yaml 路径")
    parser.add_argument("--run-id", type=str, required=True, help="本次运行 ID")
    parser.add_argument("--ork-pid", type=int, default=None, help="ork_room_server 进程 PID")
    args = parser.parse_args()

    config = _load_config(args.config)
    _run_id = args.run_id
    _ork_pid = args.ork_pid
    _start_time = time.time()

    # 结果目录
    _results_dir = Path(config.get("results_dir", "results")) / _run_id
    _results_dir.mkdir(parents=True, exist_ok=True)

    listen_port = int(config.get("monitoring", {}).get("port", 39000))

    # 信号处理
    def _sig_handler(signum: int, frame: Any) -> None:
        print("\n[INFO] Shutting down monitor...", file=sys.stderr)
        _shutdown.set()

    signal.signal(signal.SIGINT, _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    # 启动 TCP server
    server = socketserver.ThreadingTCPServer(("0.0.0.0", listen_port), WorkerReportHandler)
    server.allow_reuse_address = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    print(f"[INFO] Monitor listening on TCP :{listen_port}", file=sys.stderr)

    # 启动采集线程
    stats_thread = threading.Thread(target=_server_stats_loop, args=(60,), daemon=True)
    stats_thread.start()

    # 启动仪表盘线程
    dash_thread = threading.Thread(target=_dashboard_loop, args=(5,), daemon=True)
    dash_thread.start()

    # 主线程等待退出
    try:
        while not _shutdown.is_set():
            _shutdown.wait(1)
    except KeyboardInterrupt:
        _shutdown.set()
    finally:
        server.shutdown()
        print("[INFO] Monitor stopped.", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
