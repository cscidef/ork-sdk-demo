"""在线测试 — 验证 Reliable(KCP) 与 Unreliable(UDP) 两通道各自独立工作。

独立性判据：仅在一个通道发送 ORKBENCH1 ping，回显只应出现在该通道的计数
（`stats.bench_rudp_echoes` / `stats.bench_udp_echoes`），另一通道计数保持 0。

依赖共享 `session` fixture（见 conftest.py）。
"""

from __future__ import annotations

import time

from lib.demo_helpers import (
    RELIABLE_MARKER,
    UNRELIABLE_MARKER,
    echo_once_unreliable,
    marker_int,
    wait_echo,
)


def test_reliable_channel_only(session):
    seq = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF
    ts = time.perf_counter_ns()
    session.send_bench_reliable(seq, ts)
    hit = wait_echo(session, reliable=True, want_seq=seq, sent_ts={ts}, timeout=5.0)
    assert hit is not None, "Reliable(KCP) 通道未回显"
    assert session.stats.bench_rudp_echoes >= 1
    assert session.stats.bench_udp_echoes == 0, "UDP 通道不应因 KCP 发送而计数"


def test_unreliable_channel_only(session):
    seq = marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF
    got = echo_once_unreliable(session, seq, attempts=3, timeout=1.5)
    assert got is not None, "Unreliable(UDP) 通道未回显"
    assert session.stats.bench_udp_echoes >= 1
    assert session.stats.bench_rudp_echoes == 0, "KCP 通道不应因 UDP 发送而计数"


def test_both_channels_same_session(session):
    """同一会话内两通道交替使用，互不干扰。"""
    rseq = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF
    useq = marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF

    rts = time.perf_counter_ns()
    session.send_bench_reliable(rseq, rts)
    assert wait_echo(session, reliable=True, want_seq=rseq, sent_ts={rts}, timeout=5.0) is not None

    uhit = echo_once_unreliable(session, useq, attempts=3, timeout=1.5)
    assert uhit is not None

    assert session.stats.bench_rudp_echoes >= 1
    assert session.stats.bench_udp_echoes >= 1
