#!/usr/bin/env python3
"""Harmonia Ork 压力测试报告生成器。

读取 timeseries.jsonl + server_stats.jsonl，生成完整 Markdown 性能报告。

用法:
    python generate_stress_report.py --results results/<run_id>/ --output reports/<run_id>.md [--config stress_config.yaml]
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import platform
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import yaml

# 确保能 import 同目录下的 lib
sys.path.insert(0, str(Path(__file__).resolve().parent))
from metrics import read_jsonl  # noqa: E402


# ─── 工具函数 ────────────────────────────────────────────────────────────────

def fmt(value: Any, digits: int = 2) -> str:
    if value is None:
        return "N/A"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def safe_mean(values: list[float]) -> float | None:
    return statistics.fmean(values) if values else None


def safe_max(values: list[float]) -> float | None:
    return max(values) if values else None


def safe_min(values: list[float]) -> float | None:
    return min(values) if values else None


def safe_p95(values: list[float]) -> float | None:
    if len(values) < 2:
        return values[0] if values else None
    sorted_v = sorted(values)
    idx = int(len(sorted_v) * 0.95)
    return sorted_v[min(idx, len(sorted_v) - 1)]


def hour_bucket(ts: float) -> int:
    """返回 timestamp 所属的小时 bucket 索引（相对于 epoch 的整小时）。"""
    return int(ts // 3600)


def hour_label(bucket: int) -> str:
    return dt.datetime.fromtimestamp(bucket * 3600).strftime("%H:%M")


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, timeout=5
        ).strip()[:12]
    except Exception:
        return "unknown"


# ─── 数据聚合 ────────────────────────────────────────────────────────────────

def group_by_hour(records: list[dict[str, Any]]) -> dict[int, list[dict[str, Any]]]:
    """按小时 bucket 分组。"""
    groups: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for r in records:
        ts = r.get("timestamp", 0)
        groups[hour_bucket(ts)].append(r)
    return dict(sorted(groups.items()))


def aggregate_worker_hour(hourly: dict[int, list[dict]]) -> list[dict[str, Any]]:
    """每小时聚合 worker 数据。"""
    rows = []
    for bucket, records in hourly.items():
        row: dict[str, Any] = {"hour": hour_label(bucket), "bucket": bucket}
        row["clients_active"] = safe_max([r.get("clients_active", 0) for r in records]) or 0
        row["clients_joined"] = safe_max([r.get("clients_joined", 0) for r in records]) or 0
        row["clients_failed"] = safe_max([r.get("clients_failed", 0) for r in records]) or 0
        row["poses_sent"] = sum(r.get("poses_sent", 0) for r in records)
        row["reliable_sent"] = sum(r.get("reliable_sent", 0) for r in records)
        row["batch_sent"] = sum(r.get("batch_sent", 0) for r in records)
        row["ack_sent"] = sum(r.get("ack_sent", 0) for r in records)
        row["bytes_tx"] = sum(r.get("bytes_tx", 0) for r in records)
        row["bytes_rx"] = sum(r.get("bytes_rx", 0) for r in records)
        row["errors_count"] = sum(r.get("errors_count", 0) for r in records)
        row["reconnects"] = sum(r.get("reconnects", 0) for r in records)

        rtts = [r["avg_rtt_ms"] for r in records if r.get("avg_rtt_ms")]
        row["rtt_p50"] = safe_mean(rtts)
        row["rtt_p95"] = safe_mean([r["p95_rtt_ms"] for r in records if r.get("p95_rtt_ms")])
        row["rtt_p99"] = safe_mean([r["p99_rtt_ms"] for r in records if r.get("p99_rtt_ms")])

        mem_vals = [r["memory_rss_mb"] for r in records if r.get("memory_rss_mb")]
        row["mem_avg"] = safe_mean(mem_vals)
        row["mem_max"] = safe_max(mem_vals)
        rows.append(row)
    return rows


def aggregate_server_hour(hourly: dict[int, list[dict]]) -> list[dict[str, Any]]:
    """每小时聚合 server 数据。"""
    rows = []
    for bucket, records in hourly.items():
        row: dict[str, Any] = {"hour": hour_label(bucket), "bucket": bucket}
        cpu_vals = [r.get("cpu_pct", 0) for r in records]
        rss_vals = [r.get("rss_mb", 0) for r in records]
        row["cpu_avg"] = safe_mean(cpu_vals)
        row["cpu_max"] = safe_max(cpu_vals)
        row["rss_avg"] = safe_mean(rss_vals)
        row["rss_max"] = safe_max(rss_vals)
        row["joined_max"] = safe_max([r.get("joined_count", 0) for r in records])
        row["conn_max"] = safe_max([r.get("transport_connections", 0) for r in records])
        row["tx_mbps_avg"] = safe_mean([r.get("net_tx_mbps", 0) for r in records])
        row["rx_mbps_avg"] = safe_mean([r.get("net_rx_mbps", 0) for r in records])
        rows.append(row)
    return rows


def group_by_machine(records: list[dict]) -> dict[str, list[dict]]:
    """按 machine_ip 分组。"""
    groups: dict[str, list[dict]] = defaultdict(list)
    for r in records:
        groups[r.get("machine_ip", "unknown")].append(r)
    return dict(groups)


# ─── 报告构建 ────────────────────────────────────────────────────────────────

def build_report(
    worker_data: list[dict[str, Any]],
    server_data: list[dict[str, Any]],
    config: dict[str, Any],
    run_id: str,
    results_path: Path,
) -> str:
    generated = dt.datetime.now().astimezone().isoformat(timespec="seconds")

    # 时间范围
    timestamps_w = [r["timestamp"] for r in worker_data if "timestamp" in r]
    timestamps_s = [r["timestamp"] for r in server_data if "timestamp" in r]
    all_ts = timestamps_w + timestamps_s
    duration_s = (max(all_ts) - min(all_ts)) if all_ts else 0
    duration_str = str(dt.timedelta(seconds=int(duration_s)))

    # 聚合
    w_hourly = aggregate_worker_hour(group_by_hour(worker_data))
    s_hourly = aggregate_server_hour(group_by_hour(server_data))
    by_machine = group_by_machine(worker_data)

    # 全局统计
    total_clients = safe_max([r.get("clients_active", 0) for r in worker_data]) or 0
    total_joined = safe_max([r.get("clients_joined", 0) for r in worker_data]) or 0
    total_failed = sum(r.get("clients_failed", 0) for r in worker_data)
    total_errors = sum(r.get("errors_count", 0) for r in worker_data)
    total_poses = sum(r.get("poses_sent", 0) for r in worker_data)
    total_reliable = sum(r.get("reliable_sent", 0) for r in worker_data)
    total_batch = sum(r.get("batch_sent", 0) for r in worker_data)
    total_ack = sum(r.get("ack_sent", 0) for r in worker_data)
    total_bytes_tx = sum(r.get("bytes_tx", 0) for r in worker_data)
    total_bytes_rx = sum(r.get("bytes_rx", 0) for r in worker_data)
    total_reconnects = sum(r.get("reconnects", 0) for r in worker_data)
    join_rate = (total_joined / total_clients * 100) if total_clients > 0 else 0

    all_rtt_p50 = [r["avg_rtt_ms"] for r in worker_data if r.get("avg_rtt_ms")]
    all_rtt_p95 = [r["p95_rtt_ms"] for r in worker_data if r.get("p95_rtt_ms")]
    all_rtt_p99 = [r["p99_rtt_ms"] for r in worker_data if r.get("p99_rtt_ms")]

    lines: list[str] = []

    # ─── 标题 + 元信息 ───
    lines.extend([
        f"# Harmonia Ork 压力测试报告 — {run_id}",
        "",
        f"> 生成时间：{generated}  ",
        f"> Run ID：`{run_id}`  ",
        f"> 数据目录：`{results_path}`  ",
        f"> 测试主机：{platform.platform()} / Python {platform.python_version()}",
        "",
    ])

    if config:
        lines.extend(["### 配置快照", "", "```yaml"])
        lines.append(yaml.dump(config, default_flow_style=False, allow_unicode=True).strip())
        lines.extend(["```", ""])

    # ─── §0 执行摘要 ───
    total_msgs = total_poses + total_reliable + total_batch + total_ack
    avg_throughput = total_msgs / duration_s if duration_s > 0 else 0
    lines.extend([
        "## §0 执行摘要",
        "",
        f"| 指标 | 值 |",
        f"|------|-----|",
        f"| 总时长 | {duration_str} |",
        f"| 峰值客户端数 | {total_clients} |",
        f"| Join 成功率 | {join_rate:.1f}% |",
        f"| 总失败数 | {total_failed} |",
        f"| 总错误数 | {total_errors} |",
        f"| 总重连数 | {total_reconnects} |",
        f"| 总消息数 | {total_msgs:,} |",
        f"| 平均吞吐 | {avg_throughput:,.0f} msg/s |",
        f"| 上行总量 | {total_bytes_tx / 1e6:.1f} MB |",
        f"| 下行总量 | {total_bytes_rx / 1e6:.1f} MB |",
        "",
    ])

    # ─── §1 连接能力 ───
    lines.extend([
        "## §1 连接能力",
        "",
        "| 时段 | 活跃客户端 | 已Join | 失败 | 重连 |",
        "|------|----------:|-------:|-----:|-----:|",
    ])
    for row in w_hourly:
        lines.append(
            f"| {row['hour']} | {row['clients_active']} | {row['clients_joined']} | {row['clients_failed']} | {row['reconnects']} |"
        )
    lines.append("")

    # ─── §2 可靠消息 RTT ───
    lines.extend([
        "## §2 可靠消息 RTT",
        "",
        "| 时段 | P50 (ms) | P95 (ms) | P99 (ms) |",
        "|------|----------:|----------:|----------:|",
    ])
    for row in w_hourly:
        lines.append(f"| {row['hour']} | {fmt(row['rtt_p50'])} | {fmt(row['rtt_p95'])} | {fmt(row['rtt_p99'])} |")
    lines.extend([
        "",
        f"- 全局 RTT P50：{fmt(safe_mean(all_rtt_p50))} ms",
        f"- 全局 RTT P95：{fmt(safe_mean(all_rtt_p95))} ms",
        f"- 全局 RTT P99：{fmt(safe_mean(all_rtt_p99))} ms",
        f"- 最大 P99：{fmt(safe_max(all_rtt_p99))} ms",
        f"- 最小 P50：{fmt(safe_min(all_rtt_p50))} ms",
        "",
    ])

    # ─── §3 吞吐量 ───
    lines.extend([
        "## §3 吞吐量",
        "",
        "| 时段 | Pose/s | Reliable/s | Batch/s | Ack/s | 上行 MB | 下行 MB |",
        "|------|-------:|-----------:|--------:|------:|--------:|--------:|",
    ])
    for row in w_hourly:
        # 每小时约 3600s
        h_sec = 3600
        lines.append(
            f"| {row['hour']} | {row['poses_sent']/h_sec:,.0f} | {row['reliable_sent']/h_sec:,.0f} "
            f"| {row['batch_sent']/h_sec:,.0f} | {row['ack_sent']/h_sec:,.0f} "
            f"| {row['bytes_tx']/1e6:.1f} | {row['bytes_rx']/1e6:.1f} |"
        )

    # 消息类型分布
    if total_msgs > 0:
        lines.extend([
            "",
            "### 消息类型分布",
            "",
            f"- Pose: {total_poses:,} ({total_poses/total_msgs*100:.1f}%)",
            f"- Reliable: {total_reliable:,} ({total_reliable/total_msgs*100:.1f}%)",
            f"- Batch: {total_batch:,} ({total_batch/total_msgs*100:.1f}%)",
            f"- Ack: {total_ack:,} ({total_ack/total_msgs*100:.1f}%)",
        ])
    lines.append("")

    # ─── §4 稳定性 ───
    lines.extend([
        "## §4 稳定性",
        "",
        "### Worker 内存 RSS",
        "",
        "| 时段 | 平均 RSS (MB) | 峰值 RSS (MB) |",
        "|------|-------------:|-------------:|",
    ])
    for row in w_hourly:
        lines.append(f"| {row['hour']} | {fmt(row['mem_avg'])} | {fmt(row['mem_max'])} |")

    # 内存泄漏检测
    if len(w_hourly) >= 2:
        first_mem = w_hourly[0].get("mem_avg") or 0
        last_mem = w_hourly[-1].get("mem_avg") or 0
        growth = last_mem - first_mem
        lines.extend([
            "",
            f"- 首小时平均 RSS：{first_mem:.1f} MB",
            f"- 末小时平均 RSS：{last_mem:.1f} MB",
            f"- 增长：{growth:+.1f} MB（{'⚠️ 可能存在泄漏' if growth > 50 else '✅ 正常'}）",
        ])

    lines.extend([
        "",
        f"### 重连事件",
        "",
        f"- 总重连次数：{total_reconnects}",
        "",
        f"### 错误统计",
        "",
        f"- 总错误数：{total_errors}",
        "",
    ])

    # ─── §5 服务端健康 ───
    lines.extend([
        "## §5 服务端健康",
        "",
        "| 时段 | CPU avg% | CPU max% | RSS avg (MB) | RSS max (MB) | Joined | Connections | ↑ Mbps | ↓ Mbps |",
        "|------|--------:|---------:|-------------:|-------------:|-------:|------------:|-------:|-------:|",
    ])
    for row in s_hourly:
        lines.append(
            f"| {row['hour']} | {fmt(row['cpu_avg'],1)} | {fmt(row['cpu_max'],1)} "
            f"| {fmt(row['rss_avg'],1)} | {fmt(row['rss_max'],1)} "
            f"| {row.get('joined_max', 0)} | {row.get('conn_max', 0)} "
            f"| {fmt(row['tx_mbps_avg'],1)} | {fmt(row['rx_mbps_avg'],1)} |"
        )
    lines.append("")

    # ─── §6 分机对比 ───
    lines.extend([
        "## §6 分机对比",
        "",
    ])
    if len(by_machine) > 1:
        lines.extend([
            "| 机器 | 峰值客户端 | 总Pose | 总Reliable | 平均RTT P50 | 总错误 | 总重连 |",
            "|------|----------:|-------:|-----------:|------------:|-------:|-------:|",
        ])
        for ip, records in sorted(by_machine.items()):
            peak = safe_max([r.get("clients_active", 0) for r in records]) or 0
            poses = sum(r.get("poses_sent", 0) for r in records)
            rel = sum(r.get("reliable_sent", 0) for r in records)
            rtt = safe_mean([r["avg_rtt_ms"] for r in records if r.get("avg_rtt_ms")])
            errs = sum(r.get("errors_count", 0) for r in records)
            rec = sum(r.get("reconnects", 0) for r in records)
            lines.append(f"| {ip} | {peak} | {poses:,} | {rel:,} | {fmt(rtt)} | {errs} | {rec} |")
    else:
        lines.append("仅单机运行，无分机对比数据。")
    lines.append("")

    # ─── §7 结论 ───
    lines.extend([
        "## §7 结论",
        "",
        "### 有效结论模板",
        "",
        "> 在【客户端规模】、【持续时长】和【混合负载】下，Ork 的 Join 成功率为【数值】，"
        "可靠消息 P95 RTT 为【数值】ms。"
        "服务端 CPU 峰值【数值】%，RSS 增长【数值】MB。"
        "总错误【数值】条，重连【数值】次。",
        "",
        "### 发现的问题清单",
        "",
    ])
    issues = []
    if total_errors > 0:
        issues.append(f"- 累计错误 {total_errors} 条，需分析具体错误类型")
    if total_reconnects > total_clients * 0.1:
        issues.append(f"- 重连率偏高（{total_reconnects} 次），可能存在连接不稳定")
    if len(w_hourly) >= 2:
        first_mem = w_hourly[0].get("mem_avg") or 0
        last_mem = w_hourly[-1].get("mem_avg") or 0
        if last_mem - first_mem > 50:
            issues.append(f"- Worker 内存增长 {last_mem - first_mem:.0f} MB，疑似泄漏")
    if not issues:
        issues.append("- 未发现明显异常")
    lines.extend(issues)
    lines.append("")

    # ─── §8 复现信息 ───
    lines.extend([
        "## §8 复现信息",
        "",
        "```bash",
        f"# 监控服务",
        f"python monitor_server.py --config stress_config.yaml --run-id {run_id}",
        "",
        f"# 报告生成",
        f"python generate_stress_report.py --results results/{run_id}/ --output reports/{run_id}.md",
        "```",
        "",
        f"- Git commit: `{git_commit()}`",
        f"- 硬件: {platform.machine()} / {platform.processor() or 'N/A'}",
        f"- OS: {platform.platform()}",
        f"- Python: {platform.python_version()}",
        "",
    ])

    return "\n".join(lines)


# ─── 入口 ────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description="Harmonia Ork 压力测试报告生成器")
    parser.add_argument("--results", type=Path, required=True, help="results/<run_id>/ 目录")
    parser.add_argument("--output", type=Path, required=True, help="输出 Markdown 路径")
    parser.add_argument("--config", type=Path, default=None, help="stress_config.yaml (可选)")
    args = parser.parse_args()

    # 读取数据
    ts_file = args.results / "timeseries.jsonl"
    ss_file = args.results / "server_stats.jsonl"

    if not ts_file.exists():
        print(f"[ERROR] timeseries.jsonl not found: {ts_file}", file=sys.stderr)
        return 1

    worker_data = read_jsonl(ts_file)
    server_data = read_jsonl(ss_file) if ss_file.exists() else []

    if not worker_data:
        print("[ERROR] No worker data found.", file=sys.stderr)
        return 1

    # 加载配置
    config: dict[str, Any] = {}
    if args.config and args.config.exists():
        with open(args.config, "r", encoding="utf-8") as f:
            config = yaml.safe_load(f) or {}

    # run_id 从路径推断
    run_id = args.results.name

    # 生成报告
    report = build_report(worker_data, server_data, config, run_id, args.results)

    # 写入
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(f"[OK] Report generated: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
