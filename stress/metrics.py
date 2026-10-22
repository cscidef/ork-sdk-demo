"""Metrics schema and JSONL I/O utilities for stress testing."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, asdict, fields
from typing import Any, Dict, List


@dataclass
class WorkerReport:
    """Per-worker periodic report."""

    machine_ip: str = ""
    node_name: str = ""
    worker_id: int = 0
    timestamp: float = 0.0
    clients_active: int = 0
    clients_joined: int = 0
    clients_failed: int = 0
    poses_sent: int = 0
    reliable_sent: int = 0
    batch_sent: int = 0
    ack_sent: int = 0
    bytes_tx: int = 0
    bytes_rx: int = 0
    avg_rtt_ms: float = 0.0
    p95_rtt_ms: float = 0.0
    p99_rtt_ms: float = 0.0
    # Phase 6.3: UDP(Unreliable) 通道独立实测指标（与 benchmark 口径对齐）。
    # 默认 0，worker 在未采集 UDP probe 时保持向后兼容。
    udp_sent: int = 0
    udp_received: int = 0
    udp_reorder_count: int = 0
    udp_rtt_p50_ms: float = 0.0
    udp_rtt_p95_ms: float = 0.0
    udp_rtt_p99_ms: float = 0.0
    udp_delivery_rate: float = 0.0
    udp_loss_rate: float = 0.0
    udp_reorder_rate: float = 0.0
    errors_count: int = 0
    reconnects: int = 0
    memory_rss_mb: float = 0.0


@dataclass
class ServerStats:
    """Server-side resource and connection stats."""

    timestamp: float = 0.0
    ork_pid: int = 0
    cpu_pct: float = 0.0
    rss_mb: float = 0.0
    transport_connections: int = 0
    session_connected: int = 0
    joined_count: int = 0
    net_bytes_in: int = 0
    net_bytes_out: int = 0


def to_dict(instance: Any) -> Dict[str, Any]:
    """Convert a dataclass instance to a plain dict."""
    return asdict(instance)


def append_jsonl(path: str, data: Any) -> None:
    """Append a single JSON line to the given file.

    Args:
        path: File path for the JSONL file.
        data: A dataclass instance or dict to serialize.
    """
    record = to_dict(data) if hasattr(data, "__dataclass_fields__") else data
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".", exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")


def percentile(sorted_values: List[float], p: float) -> float:
    """Return the p-th percentile (0-100) from a pre-sorted list."""
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_values):
        return sorted_values[-1]
    d = k - f
    return sorted_values[f] * (1 - d) + sorted_values[c] * d


def compute_udp_metrics(
    udp_sent: int,
    udp_received: int,
    udp_reorder_count: int,
    udp_rtt_samples: List[float],
) -> Dict[str, float]:
    """Phase 6.3: 从 UDP(Unreliable) 原始计数/采样聚合出与 benchmark 对齐的指标。

    Args:
        udp_sent: Unreliable 通道发出的 probe 数。
        udp_received: 收到的回显数。
        udp_reorder_count: 收到 seq 非单调递增的次数。
        udp_rtt_samples: UDP RTT 采样（ms），无需预先排序。

    Returns:
        含 udp_rtt_p50/p95/p99_ms、udp_delivery_rate、udp_loss_rate、
        udp_reorder_rate 的 dict；分母为 0 时对应值为 0.0。
    """
    samples = sorted(udp_rtt_samples)
    delivery = (udp_received / udp_sent) if udp_sent else 0.0
    reorder = (udp_reorder_count / udp_received) if udp_received else 0.0
    return {
        "udp_rtt_p50_ms": round(percentile(samples, 50), 3) if samples else 0.0,
        "udp_rtt_p95_ms": round(percentile(samples, 95), 3) if samples else 0.0,
        "udp_rtt_p99_ms": round(percentile(samples, 99), 3) if samples else 0.0,
        "udp_delivery_rate": round(delivery, 6),
        "udp_loss_rate": round(1.0 - delivery, 6) if udp_sent else 0.0,
        "udp_reorder_rate": round(reorder, 6),
    }


def read_jsonl(path: str) -> List[Dict[str, Any]]:
    """Read all lines from a JSONL file and return as a list of dicts.

    Args:
        path: File path for the JSONL file.

    Returns:
        List of parsed dictionaries, one per line.
    """
    results: List[Dict[str, Any]] = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                results.append(json.loads(line))
    return results
