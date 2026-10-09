"""在线测试 — 连接并 echo（Reliable/KCP + Unreliable/UDP）。

依赖 `server` fixture 自动启停 demo 服务端。走真实传输路径：
TCP bootstrap → UDP/KCP → Handshake V2 → 双通道 25B ORKBENCH1 回显。
"""

from __future__ import annotations

import pytest

from lib.demo_helpers import (
    RELIABLE_MARKER,
    UNRELIABLE_MARKER,
    connect_verbose,
    echo_once_reliable,
    echo_once_unreliable,
    marker_int,
)


@pytest.fixture
def session(server, host, port):
    s = connect_verbose(host, port, "test-echo", quiet=True)
    try:
        yield s
    finally:
        s.close()


def test_bootstrap_assigns_connection_id(session):
    assert session.connection_id > 0
    assert session.cipher is not None


def test_reliable_echo(session):
    seq = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF
    rtt = echo_once_reliable(session, seq, timeout=5.0)
    assert rtt is not None, "Reliable(KCP) 通道未收到回显"
    assert rtt >= 0.0


def test_unreliable_echo(session):
    seq = marker_int(UNRELIABLE_MARKER) & 0xFFFF_FFFF
    rtt = echo_once_unreliable(session, seq, timeout=1.5, attempts=3)
    assert rtt is not None, "Unreliable(UDP) 通道未收到回显"


def test_multiple_reliable_echoes(session):
    base = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF
    hits = 0
    for i in range(5):
        if echo_once_reliable(session, base + i, timeout=5.0) is not None:
            hits += 1
    assert hits == 5, f"5 次 Reliable echo 仅 {hits} 次成功"
