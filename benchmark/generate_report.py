#!/usr/bin/env python3
"""Generate a self-contained Markdown comparison report from benchmark JSON.

生成链: run_benchmark.sh 采集原始 JSON → 本脚本渲染 Markdown → cp 到
reports/latest.md。可复现性元数据来自 results/<run_id>/run_metadata.json 与
resource_samples.json，缺什么就如实标注"未记录/未采集"，绝不推测补值。

附带的维护模式（由 run_benchmark.sh 调用，服务于上述元数据）:
  --sample <file> --tag <name>        在构建/测试前后采样 load/memory/swap 并追加
  --emit-meta <file> --build-dir DIR  记录 build type / git commit / CPU 等元数据
                                       (--bench-command 传入实际调用命令)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


LABELS = {
    "tcp_only": "TCP-only",
    "udp_only": "UDP-only",
    "tcp_udp": "传统 TCP+UDP",
    "ork_hybrid": "Ork Hybrid",
}

META_FILE = "run_metadata.json"
RESOURCE_FILE = "resource_samples.json"
NOT_RECORDED = "未记录"
NOT_SAMPLED = "未采集"
RESOURCE_TAGS = ("build_before", "build_after", "bench_before", "bench_after")
RESOURCE_TAG_LABELS = {
    "build_before": "构建前",
    "build_after": "构建后",
    "bench_before": "benchmark 前",
    "bench_after": "benchmark 后",
}


def fmt(value: Any, digits: int = 2) -> str:
    if value is None:
        return "N/A"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def percent(value: Any) -> str:
    return "N/A" if value is None else f"{float(value):.2f}%"


def fmt_dash(value: Any, digits: int = 2) -> str:
    """Phase 6.3: UDP 列专用格式化——数据为空/N/A 时显示“—”而非 0。"""
    return "—" if value is None else f"{float(value):.{digits}f}"


def rate_dash(value: Any) -> str:
    """Phase 6.3: 将 0..1 比率渲染为百分比；空值显示“—”。"""
    return "—" if value is None else f"{float(value) * 100.0:.2f}%"


def mean(rows: list[dict[str, Any]], key: str) -> float | None:
    values = [float(row[key]) for row in rows if row.get(key) is not None]
    return statistics.fmean(values) if values else None


def aggregate(files: list[Path]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for file in files:
        record = json.loads(file.read_text(encoding="utf-8"))
        key = (record["transport"], record["network"], record["clients"], record["duration_s"])
        groups.setdefault(key, []).append(record)

    output = []
    fields = (
        "connect_success_pct", "connect_ms_p95", "reliable_delivery_pct",
        "reliable_rtt_ms_p50", "reliable_rtt_ms_p95", "reliable_rtt_ms_p99",
        "realtime_delivery_pct", "realtime_rtt_ms_p95", "offered_mbps",
        # Phase 6.3: UDP(Unreliable) 通道实测指标
        "udp_rtt_p50", "udp_rtt_p95", "udp_rtt_p99",
        "udp_delivery_rate", "udp_loss_rate", "udp_reorder_rate",
    )
    for key, records in sorted(groups.items()):
        row = {"transport": key[0], "network": key[1], "clients": key[2],
               "duration_s": key[3], "repetitions": len(records)}
        row.update({field: mean(records, field) for field in fields})
        first = records[0]
        for param in ("pose_hz", "reliable_hz", "payload_bytes", "bone_count"):
            row[param] = first.get(param)
        row["errors"] = sum(len(record.get("errors", [])) for record in records)
        row["handshake_retries"] = sum(int(record.get("handshake_retries", 0)) for record in records)
        output.append(row)
    return output


# --------------------------------------------------------------------------
# 元数据 / 资源采样辅助（均为只读查询，不修改任何被测对象）
# --------------------------------------------------------------------------

def load_json_if_exists(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def detect_encryption(results_dir: Path) -> str | None:
    """从 Ork 服务器日志提取 encryption 开关（true/false），无则 None。"""
    try:
        text = (results_dir / "ork_room_server.log").read_text(
            encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(
        r"SessionFramework started \([^)]*encryption=(true|false)", text)
    return match.group(1) if match else None


def sysctl(name: str) -> str:
    try:
        return subprocess.run(
            ["sysctl", "-n", name], capture_output=True, text=True,
            check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def git_info(repo_root: Path) -> tuple[str, str]:
    """返回 (commit, subject)，git 不可用时为空字符串。"""
    def run(*args: str) -> str:
        try:
            return subprocess.run(
                ["git", "-C", str(repo_root), *args], capture_output=True,
                text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return ""
    return run("rev-parse", "HEAD"), run("log", "-1", "--pretty=%s")


def cpu_info() -> dict[str, Any]:
    """只读查询 CPU / 硬件标识；macOS 用 sysctl，Linux 读 /proc。"""
    info: dict[str, Any] = {}
    if sys.platform == "darwin":
        info["cpu_model"] = sysctl("machdep.cpu.brand_string") or None
        info["hardware_model"] = sysctl("hw.model") or None
        ram = sysctl("hw.memsize")
        info["ram_bytes"] = int(ram) if ram.isdigit() else None
        cores = sysctl("hw.ncpu")
        info["cpu_cores"] = int(cores) if cores.isdigit() else None
    else:
        info["cpu_model"] = None
        info["hardware_model"] = None
        info["ram_bytes"] = None
        info["cpu_cores"] = os.cpu_count()
        try:
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    info["cpu_model"] = line.split(":", 1)[1].strip()
                    break
        except OSError:
            pass
    return info


def _macos_memory_stats() -> dict[str, int]:
    try:
        out = subprocess.run(
            ["vm_stat"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return {}
    lines = out.splitlines()
    page_size = 16384
    if lines and "page size of" in lines[0]:
        digits = re.sub(r"\D", "", lines[0].split("page size of")[1])
        if digits:
            page_size = int(digits)
    pages: dict[str, int] = {}
    for line in lines[1:]:
        match = re.match(r"Pages\s+([\w\s]+?):\s+(\d+)", line)
        if match:
            pages[match.group(1).strip()] = int(match.group(2)) * page_size
    available = (pages.get("free", 0) + pages.get("speculative", 0)
                 + pages.get("inactive", 0) + pages.get("purgeable", 0))
    stats = {"mem_available_bytes": available}
    total = sysctl("hw.memsize")
    if total.isdigit():
        stats["mem_total_bytes"] = int(total)
    return stats


def _macos_swap_used_bytes() -> int | None:
    usage = sysctl("vm.swapusage")
    match = re.search(r"used\s*[:=]\s*([0-9.]+)\s*([BKMGT]?)", usage)
    if not match:
        return None
    scale = {"": 1, "B": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}
    return int(float(match.group(1)) * scale[match.group(2)])


def _linux_memory_swap() -> dict[str, int]:
    stats: dict[str, int] = {}
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, _, rest = line.partition(":")
            value = rest.strip().split()
            if key in ("MemTotal", "MemAvailable", "SwapTotal", "SwapFree") and value:
                stats[key] = int(value[0]) * 1024
    except (OSError, ValueError):
        pass
    return stats


def sample_system_resources() -> dict[str, Any]:
    """只读采样 load average / 可用内存 / swap，尽力而为、不抛异常。"""
    sample: dict[str, Any] = {
        "timestamp": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
    }
    try:
        loads = os.getloadavg()
        sample["load_avg"] = [round(value, 2) for value in loads]
    except OSError:
        pass
    if sys.platform == "darwin":
        sample.update(_macos_memory_stats())
        swap_used = _macos_swap_used_bytes()
        if swap_used is not None:
            sample["swap_used_bytes"] = swap_used
    else:
        info = _linux_memory_swap()
        if "MemTotal" in info:
            sample["mem_total_bytes"] = info["MemTotal"]
        if "MemAvailable" in info:
            sample["mem_available_bytes"] = info["MemAvailable"]
        if "SwapTotal" in info and "SwapFree" in info:
            sample["swap_used_bytes"] = max(0, info["SwapTotal"] - info["SwapFree"])
    return sample


def append_resource_sample(path: Path, tag: str) -> None:
    data = load_json_if_exists(path) or {}
    data[tag] = sample_system_resources()
    path.write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_build_type(build_dir: Path) -> str | None:
    try:
        text = (build_dir / "CMakeCache.txt").read_text(
            encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", text, re.MULTILINE)
    value = match.group(1).strip() if match else ""
    return value or None


def emit_metadata(path: Path, build_dir: Path, bench_command: str) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    commit, subject = git_info(repo_root)
    meta: dict[str, Any] = {
        "schema_version": 1,
        "collected_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "git_commit": commit or None,
        "git_commit_subject": subject or None,
        "build_type": read_build_type(build_dir),
        "build_dir": str(build_dir) if str(build_dir) else None,
        "benchmark_command": bench_command or None,
        "python_version": platform.python_version(),
    }
    meta.update(cpu_info())
    path.write_text(
        json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------
# 报告渲染
# --------------------------------------------------------------------------

def _format_gib(value: Any) -> str:
    if value is None:
        return NOT_RECORDED
    return f"{value / (1024 ** 3):.2f} GB"


def _render_resource_cell(sample: dict[str, Any] | None) -> str:
    if sample is None:
        return f"{NOT_SAMPLED} | {NOT_SAMPLED} | {NOT_SAMPLED}"
    loads = sample.get("load_avg")
    load_text = " / ".join(f"{value:.2f}" for value in loads) if loads else NOT_RECORDED
    mem_avail = sample.get("mem_available_bytes")
    mem_total = sample.get("mem_total_bytes")
    if mem_avail is not None:
        mem_text = f"可用 {_format_gib(mem_avail)}"
        if mem_total is not None:
            mem_text += f" / 共 {_format_gib(mem_total)}"
    else:
        mem_text = NOT_RECORDED
    swap_text = _format_gib(sample.get("swap_used_bytes"))
    return f"{load_text} | {mem_text} | {swap_text}"


def _meta_phase4_params(meta: dict[str, Any] | None, row: dict[str, Any] | None) -> dict[str, Any]:
    """优先使用补录 metadata 中的 phase4_env；否则从本次采集参数构造。"""
    if meta and isinstance(meta.get("phase4_env"), dict) and meta["phase4_env"]:
        return meta["phase4_env"]
    if row:
        return {
            "NETWORK": row.get("network"),
            "CLIENTS": row.get("clients"),
            "DURATION_S": row.get("duration_s"),
            "REPETITIONS": row.get("repetitions"),
            "POSE_HZ": row.get("pose_hz"),
            "RELIABLE_HZ": row.get("reliable_hz"),
            "PAYLOAD_BYTES": row.get("payload_bytes"),
            "BONE_COUNT": row.get("bone_count"),
        }
    return {}


def _meta_section(meta: dict[str, Any] | None,
                  resources: dict[str, Any] | None,
                  encryption: str | None,
                  row: dict[str, Any] | None) -> list[str]:
    meta = meta or {}

    commit = meta.get("git_commit")
    if commit:
        subject = meta.get("git_commit_subject")
        revision = f"`{str(commit)[:12]}`" + (f"（{subject}）" if subject else "")
    else:
        revision = NOT_RECORDED

    cpu_model = meta.get("cpu_model") or NOT_RECORDED
    cores = meta.get("cpu_cores")
    cpu_text = cpu_model + (f"，{cores} cores" if cores else "")

    hardware = meta.get("hardware_model")
    ram_bytes = meta.get("ram_bytes")
    hardware_text = hardware or NOT_RECORDED
    if ram_bytes:
        hardware_text += f" / {ram_bytes / (1024 ** 3):.0f} GB"

    encryption_text = {
        "true": "enabled（服务器日志 SessionFramework encryption=true）",
        "false": "disabled（服务器日志 SessionFramework encryption=false）",
    }.get(encryption or "", NOT_RECORDED)

    topology = meta.get("topology") or "同机回环 127.0.0.1（服务器与客户端同主机）"
    collected_at = meta.get("collected_at") or NOT_RECORDED
    python_version = meta.get("python_version") or platform.python_version()

    lines = [
        "## 6. 可复现性与环境元数据",
        "",
        "| 项目 | 值 |",
        "|---|---|",
        f"| Build Type | {meta.get('build_type') or NOT_RECORDED} |",
        f"| Source revision | {revision} |",
        f"| CPU | {cpu_text} |",
        f"| 硬件 / 内存 | {hardware_text} |",
        f"| Python | {python_version} |",
        f"| 加密 | {encryption_text} |",
        f"| 拓扑 | {topology} |",
        f"| 数据采集时段 | {collected_at} |",
        "",
        "> 以上字段来自 `results/<run_id>/run_metadata.json`（历史基线为按 CMakeCache / git / sysctl / 服务器日志核实后补录，可复核；后续运行由 `run_benchmark.sh` 自动采集）。缺失项如实标注，不以事后查询值补录。",
        "",
        "### 6.1 固定参数",
        "",
    ]

    params = _meta_phase4_params(meta, row)
    if params:
        lines.append("| 参数 | 值 |")
        lines.append("|---|---|")
        for key, value in params.items():
            lines.append(f"| {key} | {value} |")
        source = ("校验基线 profile（与原始 JSON 参数一致）"
                  if meta.get("phase4_env") else "本次采集实际参数（原始 JSON）")
        lines.append("")
        lines.append(f"> 参数来源：{source}。")
    else:
        lines.append(NOT_RECORDED + "。")
    lines.append("")

    lines.append("### 6.2 构建目录与可复现命令")
    lines.append("")
    build_dir = meta.get("build_dir")
    build_type = meta.get("build_type")
    configure = meta.get("cmake_configure_command") or (
        f"cmake -S . -B {build_dir} -DCMAKE_BUILD_TYPE={build_type}"
        if build_dir and build_type else None)
    build_command = meta.get("cmake_build_command") or (
        f"bash scripts/pack_server.sh  # 打包 server/<platform>/bin/embed_minimal" if build_dir else None)
    bench_command = meta.get("benchmark_command")
    if configure or build_command or bench_command:
        lines.append("```bash")
        if configure:
            lines.append(f"# 构建（等效命令，参数与 CMakeCache 一致）")
            lines.append(configure)
        if build_command:
            lines.append(build_command)
        if bench_command:
            lines.append("")
            lines.append("# 采集（等效复现命令，参数与原始数据一致）")
            lines.append(f"cd benchmark")
            lines.append(bench_command)
        lines.append("```")
    else:
        lines.append(NOT_RECORDED + "。")
    if build_dir:
        lines.append("")
        lines.append(f"> 构建目录：`{build_dir}`（Build Type 以其 `CMakeCache.txt` 为准）。")
    lines.append("")

    lines.append("### 6.3 系统资源采样（构建 / benchmark 前后）")
    lines.append("")
    lines.append("| 时点 | Load avg (1m/5m/15m) | 内存 | Swap 使用 |")
    lines.append("|---|---|---|---|")
    resources = resources or {}
    for tag in RESOURCE_TAGS:
        label = RESOURCE_TAG_LABELS[tag]
        lines.append(f"| {label} | {_render_resource_cell(resources.get(tag))} |")
    if not resources:
        lines.append("")
        lines.append(
            "> 本次基线运行未留存系统级资源采样（生成链当时未含该步骤），上表如实标注"
            f"{NOT_SAMPLED}，不以事后查询值补录。生成链已升级：后续 `run_benchmark.sh` "
            "会在构建与测试前后自动采样 load/memory/swap 并写入 "
            "`results/<run_id>/resource_samples.json`。")
    lines.append("")

    lines.append("### 6.4 外推边界")
    lines.append("")
    lines.append("- 全部数据来自单台主机的本机回环（127.0.0.1）：无网络损耗、无丢包、无跨机抖动，绝对延迟数值不可外推到公网、Wi-Fi、跨机房或弱网环境。")
    lines.append("- 服务器与客户端同机共享 CPU / 内存，资源竞争模式与生产分机部署不同。")
    lines.append("- 弱网与跨机结论必须以独立 Linux 网关 `netem.sh`（`BENCH_NETWORK` 标记实际环境）的结果为准。")
    lines.append("")
    return lines


def build_report(rows: list[dict[str, Any]], source: Path,
                 meta: dict[str, Any] | None = None,
                 resources: dict[str, Any] | None = None,
                 encryption: str | None = None) -> str:
    generated = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    ork_rows = [row for row in rows if row["transport"] == "ork_hybrid"]
    ork_connections = mean(ork_rows, "connect_success_pct")
    ork_delivery = mean(ork_rows, "reliable_delivery_pct")
    ork_errors = sum(int(row["errors"]) for row in ork_rows)
    ork_retries = sum(int(row["handshake_retries"]) for row in ork_rows)
    lines = [
        "# Harmonia Ork 通讯方案对比测试报告",
        "",
        f"> 生成时间：{generated}  ",
        f"> 原始数据：`{source}`  ",
        f"> 测试主机：{platform.platform()} / Python {platform.python_version()}",
        "",
        "## 0. 执行摘要",
        "",
        f"- Ork 各并发档位的平均连接成功率：{percent(ork_connections)}。",
        f"- Ork 可靠消息各组平均送达率：{percent(ork_delivery)}。",
        f"- Ork 记录到握手重试 {ork_retries} 次、客户端错误 {ork_errors} 条。",
        "- 本机无损回环主要验证功能、稳定性和容量趋势；传输方案优劣必须结合弱网结果判断。",
        "- reference server 是最小 echo 下界。若 Ork RTT 高于它，代表完整协议栈成本，不等同于游戏体验更差；若要证明混合通道价值，应关注丢包环境下可靠消息尾延迟和实时消息新鲜度。",
        "- 本次基线的 Build Type、source revision、CPU/硬件标识、固定参数、复现命令、资源采样与外推边界见第 6 节。",
        "",
        "## 1. 测试目的",
        "",
        "在统一并发数、消息频率和测试时长下，对比 TCP-only、UDP-only、传统 TCP+UDP 与 Harmonia Ork Hybrid。报告关注连接成功率、可靠消息 RTT、消息送达率和业务负载吞吐。",
        "",
        "## 2. 被测实现与公平性边界",
        "",
        "| 方案 | 实现 | 说明 |",
        "|---|---|---|",
        "| TCP-only | C++ reference echo | 可靠与实时消息都经过 TCP |",
        "| UDP-only | C++ reference echo | 所有消息经过裸 UDP |",
        "| 传统 TCP+UDP | C++ reference echo | 可靠消息走 TCP，实时消息走 UDP |",
        "| Ork Hybrid | 真实 ork_room_server | TCP bootstrap、RUDP/KCP、UDP、Protobuf、会话、Tick 与配置中的加密策略 |",
        "",
        '> 重要：reference server 是传输下界，不是完整传统游戏服务器。Ork 承担更多协议与会话工作，因此本报告不会把单次本机结果泛化成"任何环境都更快"。Phase 4.0 起 Ork 使用 ORKBENCH1 opaque ping 同时测量 Reliable 和 Unreliable 通道的 RTT 与送达率。',
        "",
        "## 3. 汇总结果",
        "",
        "| 网络 | 客户端 | 方案 | 重复 | 连接成功率 | Join/连接 P95 ms | 可靠送达率 | 可靠 RTT P50/P95/P99 ms | 实时送达率 | 实时 RTT P95 ms | UDP RTT P50 ms | UDP RTT P95 ms | UDP RTT P99 ms | UDP 送达率 | Offered Mbps | 握手重试 | 错误 |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        rtt = "/".join(fmt(row[key]) for key in (
            "reliable_rtt_ms_p50", "reliable_rtt_ms_p95", "reliable_rtt_ms_p99"))
        lines.append(
            f"| {row['network']} | {row['clients']} | {LABELS.get(row['transport'], row['transport'])} "
            f"| {row['repetitions']} | {percent(row['connect_success_pct'])} | {fmt(row['connect_ms_p95'])} "
            f"| {percent(row['reliable_delivery_pct'])} | {rtt} | {percent(row['realtime_delivery_pct'])} "
            f"| {fmt(row['realtime_rtt_ms_p95'])} "
            f"| {fmt_dash(row.get('udp_rtt_p50'))} | {fmt_dash(row.get('udp_rtt_p95'))} "
            f"| {fmt_dash(row.get('udp_rtt_p99'))} | {rate_dash(row.get('udp_delivery_rate'))} "
            f"| {fmt(row['offered_mbps'], 3)} "
            f"| {row['handshake_retries']} | {row['errors']} |"
        )

    lines.extend([
        "",
        "## 4. 结果判读",
        "",
        "正式结论应以至少 5 次独立重复的均值和波动范围为依据，并重点观察 P95/P99，而不是只看平均延迟。建议按以下规则判读：",
        "",
        "- 实时性：可靠 RTT P95/P99 越低越好；TCP 在丢包下可能出现队头阻塞。",
        "- 可靠性：可靠送达率应接近 100%；UDP-only 的结果不代表业务可靠语义。",
        "- 可用性：连接成功率和错误数用于识别容量拐点。",
        "- 带宽：Offered Mbps 是客户端产生的业务负载，不等于网卡实际字节；正式测试应补充系统网络计数器。",
        "- 负载大小：reference 的 `payload_bytes` 是固定 wire payload；Ork 使用真实 Protobuf 游戏消息，大小由字段和加密帧决定，因此 Offered Mbps 只反映各方案实际产生的负载，不应直接解释成协议开销率。",
        "- Ork 实时通道：Phase 4.0 起使用 ORKBENCH1 opaque ping 测量 UDP 端到端 RTT，不再是 N/A；结果反映本机回环下完整协议栈（加密帧、UDP 发送/接收、OrkFrame 解包）的往返延迟。",
        "- UDP 实测列（Phase 6.3）：`UDP RTT P50/P95/P99` 与 `UDP 送达率` 来自 Unreliable 通道 ORKBENCH1 往返，送达率=received/sent；原始 JSON 另含 `udp_loss_rate`（1-送达率）与 `udp_reorder_rate`（收到 seq 非单调递增的比例）。本机无损回环下 UDP 送达率应 ≥99%，且 `udp_rtt_p50` 应低于 `rudp_rtt_p50`（UDP 无重传/拥塞控制）。",
        "- UDP 列显示 “—” 表示该方案无独立 UDP 通道（如 TCP-only，其实时消息也走 TCP），非数值 0。",
        "",
        "## 5. 有效结论模板",
        "",
        "请基于表格填写，不要预先写入‘Ork 全面优于所有方案’：",
        "",
        "> 在【网络条件】、【客户端数】和【混合负载】下，Ork 的可靠消息 P95/P99 RTT 为【数值】，可靠送达率为【数值】。相较【基线】，改善/退化【数值】；同时 Ork 提供连接生命周期、可靠/不可靠分流、加密和 Tick 集成。其代价或未覆盖项为【CPU/带宽/UDP 回显限制】。",
        "",
    ])

    reference_row = rows[0] if rows else None
    lines.extend(_meta_section(meta, resources, encryption, reference_row))

    lines.extend([
        "## 7. 弱网与后续要求",
        "",
        "弱网测试应在独立 Linux 网关使用 `netem.sh`，并通过 `BENCH_NETWORK` 标记实际环境。CPU 型号、同机/分机拓扑、Release 编译参数、git commit 与加密开关已在第 6 节记录；网卡实际流量计数器本次未留存，正式对外报告应补充系统网络计数器数据。",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate benchmark report / collect reproducibility metadata.")
    parser.add_argument("--results", type=Path, help="原始 JSON 所在目录（报告模式）")
    parser.add_argument("--output", type=Path, help="报告输出路径（报告模式）")
    parser.add_argument("--sample", type=Path, help="维护模式：采样并追加到该 JSON 文件")
    parser.add_argument("--tag", help="配合 --sample：采样时点标签")
    parser.add_argument("--emit-meta", type=Path, dest="emit_meta",
                        help="维护模式：生成 run_metadata.json 到该路径")
    parser.add_argument("--build-dir", type=Path, dest="build_dir",
                        help="配合 --emit-meta：Ork 构建目录")
    parser.add_argument("--bench-command", dest="bench_command", default="",
                        help="配合 --emit-meta：实际 benchmark 调用命令")
    args = parser.parse_args()

    if args.sample:
        if not args.tag:
            parser.error("--sample 需要同时提供 --tag")
        args.sample.parent.mkdir(parents=True, exist_ok=True)
        append_resource_sample(args.sample, args.tag)
        print(f"resource sample appended: {args.sample} [{args.tag}]")
        return 0

    if args.emit_meta:
        emit_metadata(args.emit_meta, args.build_dir or Path(), args.bench_command)
        print(f"metadata emitted: {args.emit_meta}")
        return 0

    if not args.results or not args.output:
        parser.error("报告模式需要 --results 与 --output")
    files = sorted(path for path in args.results.glob("*.json")
                   if path.name not in (META_FILE, RESOURCE_FILE))
    if not files:
        parser.error(f"no JSON results found in {args.results}")
    meta = load_json_if_exists(args.results / META_FILE)
    resources = load_json_if_exists(args.results / RESOURCE_FILE)
    encryption = detect_encryption(args.results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        build_report(aggregate(files), args.results, meta, resources, encryption),
        encoding="utf-8")
    print(f"report generated: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
