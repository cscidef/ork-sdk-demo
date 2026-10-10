"""Demo 共享辅助函数 — 连接编排、ORKBENCH1 双通道 echo、RTT 统计。

demos/ 下的 6 个示例脚本复用这里的薄封装，保持每个 client.py 聚焦于自身演示
主题；所有收发仍走 lib.ork_client_session 的真实传输路径（无模拟）。

回显约束：标准 demo 服务端仅回显 **精确 25B、前缀 "ORKBENCH1"** 的 opaque 帧，
故 echo 一律通过 send_bench_* / drain_bench_* 完成。
"""

from __future__ import annotations

import statistics
import sys
import time
from dataclasses import dataclass, field

from .ork_client_session import OrkClientSession

# 双通道 echo 标记（嵌入 25B ORK 帧的 seq 槽位，逐字段校验等价整帧校验）
RELIABLE_MARKER = b"ORKDEMO1"    # Reliable (KCP)
UNRELIABLE_MARKER = b"ORKDEMO2"  # Unreliable (UDP)
ECHO_FRAME_SIZE = 25

RELIABLE_ECHO_TIMEOUT = 5.0   # KCP 可靠通道单轮等待（秒）
UDP_ECHO_TIMEOUT = 1.0        # UDP 不可靠通道单次尝试等待（秒）
UDP_ECHO_ATTEMPTS = 3         # UDP 允许少量重发
PER_ITER_TIMEOUT = 1.0        # run_channel 单次发送的回显等待窗口（秒）


def marker_int(marker: bytes) -> int:
    """marker 8B → little-endian uint64（填入 bench payload 的 sequence 槽位）。"""
    return int.from_bytes(marker, "little")


# --------------------------------------------------------------------------- #
# 输出美化（CLI）
# --------------------------------------------------------------------------- #

_WIDTH = 68


def banner(title: str, subtitle: str | None = None) -> None:
    """打印演示标题横幅。"""
    print()
    print("┌" + "─" * (_WIDTH - 2) + "┐")
    print("│ " + title.ljust(_WIDTH - 4) + " │")
    if subtitle:
        print("│ " + subtitle.ljust(_WIDTH - 4) + " │")
    print("└" + "─" * (_WIDTH - 2) + "┘")


def rule(label: str = "") -> None:
    """打印分隔线（可带居中标签）。"""
    if not label:
        print("─" * _WIDTH)
        return
    pad = _WIDTH - len(label) - 2
    left = pad // 2
    right = pad - left
    print("─" * left + " " + label + " " + "─" * right)


def ok(msg: str) -> None:
    print(f"  \033[32m✓\033[0m {msg}")


def fail(msg: str) -> None:
    print(f"  \033[31m✗\033[0m {msg}", file=sys.stderr)


def info(msg: str) -> None:
    print(f"  · {msg}")


# --------------------------------------------------------------------------- #
# 连接编排
# --------------------------------------------------------------------------- #


def connect_verbose(host: str, port: int, name: str = "ork-demo", *, quiet: bool = False) -> OrkClientSession:
    """建立完整连接并（默认）打印四步骤进度；返回已连接的会话。

    步骤：TCP bootstrap(109B) → UDP+KCP → Handshake V2(42B) → wait "ORK-UDP-ACK:"
    """
    session = OrkClientSession(host, port, name)
    if not quiet:
        print(f"[1/4] TCP 连接 + Bootstrap V2 (109B opaque)  →  {host}:{port}")
    summary = session.connect_tcp_bootstrap()
    if not quiet:
        ok(f"{summary}")
        print("[2/4] 绑定 UDP socket + 创建 KCP 会话 (conv = connectionId)")
    session.start_udp_rudp()
    if not quiet:
        ok("UDP 已绑定，KCP 会话已创建")
        print(f"[3/4] 发送 UDP Handshake V2 (42B 认证帧)  conn={session.connection_id}")
    session.send_udp_handshake()
    if not quiet:
        ok("handshake 已发送")
        print("[4/4] 等待 KCP opaque ACK ...")
    ack = session.wait_udp_ack()
    if not quiet:
        ok(f"{ack}  (加密模式: {'AEAD ChaCha20-Poly1305' if session.encryption_required else 'plaintext'})")
        rule("连接就绪")
    return session


# --------------------------------------------------------------------------- #
# ORKBENCH1 双通道 echo
# --------------------------------------------------------------------------- #


def wait_echo(
    session: OrkClientSession,
    *,
    reliable: bool,
    want_seq: int,
    sent_ts: set[int],
    timeout: float,
) -> int | None:
    """轮询回显直到 (seq, ts) 匹配或超时；返回命中的 send_time_ns 或 None。"""
    deadline = time.monotonic() + timeout
    while True:
        session.pump_rudp()  # 驱动 KCP 收发（UDP 回显由后台线程入队）
        echoes = session.drain_bench_rudp() if reliable else session.drain_bench_udp()
        for seq, ts in echoes:
            if seq == want_seq and ts in sent_ts:
                return ts
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.002)


def echo_once_reliable(session: OrkClientSession, seq: int, *, timeout: float = RELIABLE_ECHO_TIMEOUT) -> float | None:
    """Reliable(KCP) 单发单收；成功返回 RTT(ms)，失败返回 None。"""
    ts = time.perf_counter_ns()
    session.send_bench_reliable(seq, ts)
    hit = wait_echo(session, reliable=True, want_seq=seq, sent_ts={ts}, timeout=timeout)
    if hit is None:
        return None
    return (time.perf_counter_ns() - hit) / 1e6


def echo_once_unreliable(
    session: OrkClientSession,
    seq: int,
    *,
    timeout: float = UDP_ECHO_TIMEOUT,
    attempts: int = 1,
) -> float | None:
    """Unreliable(UDP) 单发单收；attempts>1 时允许重发。成功返回 RTT(ms)。"""
    sent_ts: set[int] = set()
    for _ in range(max(1, attempts)):
        ts = time.perf_counter_ns()
        sent_ts.add(ts)
        session.send_bench_unreliable(seq, ts)
        hit = wait_echo(session, reliable=False, want_seq=seq, sent_ts=sent_ts, timeout=timeout)
        if hit is not None:
            return (time.perf_counter_ns() - hit) / 1e6
    return None


# --------------------------------------------------------------------------- #
# 单通道批量 echo（送达率 + RTT 采样）
# --------------------------------------------------------------------------- #


@dataclass
class ChannelResult:
    """run_channel 的结果：按字段名访问，杜绝 (delivered, rtts) 顺序误赋值。"""

    delivered: int = 0
    rtts: list[float] = field(default_factory=list)


def run_channel(
    session: OrkClientSession,
    *,
    reliable: bool,
    base_seq: int,
    iterations: int,
    warmup: int = 0,
    per_iter_timeout: float = PER_ITER_TIMEOUT,
) -> ChannelResult:
    """在单个通道上单发单收 iterations 次，收集送达计数与 RTT(ms) 采样。

    统一 demo 02（送达率对比）与 demo 06（时延测量）此前各自实现、返回顺序相反
    的两套逻辑，避免 (delivered, rtts) / (rtts, delivered) 静默错配。

    Args:
        reliable: True 走 Reliable(KCP)，False 走 Unreliable(UDP)。
        base_seq: 首个 seq；后续按 base_seq + i 递增。
        iterations: 发送次数。
        warmup: 前若干次不计入 RTT 样本（仍计入 delivered）；默认 0。
        per_iter_timeout: 单次发送的回显等待窗口（秒）。
    """
    result = ChannelResult()
    for i in range(iterations):
        seq = base_seq + i
        ts = time.perf_counter_ns()
        if reliable:
            session.send_bench_reliable(seq, ts)
        else:
            session.send_bench_unreliable(seq, ts)
        hit = wait_echo(session, reliable=reliable, want_seq=seq,
                        sent_ts={ts}, timeout=per_iter_timeout)
        if hit is not None:
            result.delivered += 1
            if i >= warmup:
                result.rtts.append((time.perf_counter_ns() - hit) / 1e6)
    return result


# --------------------------------------------------------------------------- #
# RTT 统计
# --------------------------------------------------------------------------- #


def percentile(sorted_vals: list[float], pct: float) -> float:
    """最近秩法百分位（sorted_vals 已升序）；空列表返回 0.0。"""
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * (pct / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = k - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def summarize_rtt(samples: list[float]) -> dict:
    """给定 RTT(ms) 采样，返回 count/min/max/avg/p50/p95/p99 统计。"""
    if not samples:
        return {"count": 0, "min": 0.0, "max": 0.0, "avg": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0}
    s = sorted(samples)
    return {
        "count": len(s),
        "min": s[0],
        "max": s[-1],
        "avg": statistics.fmean(s),
        "p50": percentile(s, 50),
        "p95": percentile(s, 95),
        "p99": percentile(s, 99),
    }


def format_rtt_table(rows: dict[str, dict]) -> str:
    """把 {通道名: summarize_rtt 结果} 渲染成对齐的文本表格。"""
    headers = ["channel", "count", "min", "avg", "p50", "p95", "p99", "max"]
    lines = []
    col_w = [16, 6, 9, 9, 9, 9, 9, 9]
    lines.append("  " + " ".join(h.rjust(w) for h, w in zip(headers, col_w)))
    lines.append("  " + " ".join("─" * w for w in col_w))
    for name, st in rows.items():
        cells = [
            name.rjust(col_w[0]),
            str(st["count"]).rjust(col_w[1]),
            f"{st['min']:.3f}".rjust(col_w[2]),
            f"{st['avg']:.3f}".rjust(col_w[3]),
            f"{st['p50']:.3f}".rjust(col_w[4]),
            f"{st['p95']:.3f}".rjust(col_w[5]),
            f"{st['p99']:.3f}".rjust(col_w[6]),
            f"{st['max']:.3f}".rjust(col_w[7]),
        ]
        lines.append("  " + " ".join(cells))
    lines.append("  " + "(单位: ms)")
    return "\n".join(lines)


def add_common_args(parser) -> None:
    """为 demo  argparse 添加通用 --host / --port。"""
    parser.add_argument("--host", default="127.0.0.1", help="服务端地址 (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9400, help="游戏端口，TCP bootstrap 与 UDP/KCP 共用 (default: 9400)")
