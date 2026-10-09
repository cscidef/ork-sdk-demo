"""在线测试 — 连接生命周期：connect → send → disconnect → reconnect。

验证：
- 首次连接后 echo 正常
- close() 释放资源（socket / KCP 会话置空），且幂等（二次 close 不抛）
- 断开后重连得到新的可用会话，echo 仍正常
- 重连分配新的 connectionId（服务端为每条 TCP bootstrap 分配）
"""

from __future__ import annotations

from lib.demo_helpers import (
    RELIABLE_MARKER,
    connect_verbose,
    echo_once_reliable,
    marker_int,
)
from lib.ork_client_session import OrkClientSession


def test_connect_send_disconnect_reconnect(server, host, port):
    seq_base = marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF

    # ① 首次连接 + 发送
    s1 = connect_verbose(host, port, "test-lifecycle-1", quiet=True)
    conn1 = s1.connection_id
    assert conn1 > 0
    assert echo_once_reliable(s1, seq_base, timeout=5.0) is not None

    # ② 断开：资源释放 + 幂等
    s1.close()
    assert s1._tcp_sock is None
    assert s1._udp_sock is None
    assert s1._rudp is None
    s1.close()  # 二次 close 不应抛异常

    # ③ 重连：新会话可用
    s2 = connect_verbose(host, port, "test-lifecycle-2", quiet=True)
    try:
        conn2 = s2.connection_id
        assert conn2 > 0
        assert conn2 != conn1, "重连应得到新的 connectionId"
        assert echo_once_reliable(s2, seq_base + 1, timeout=5.0) is not None
    finally:
        s2.close()


def test_context_manager_closes(server, host, port):
    """with 语句退出时自动 close。"""
    with OrkClientSession(host, port, "test-lifecycle-ctx") as s:
        s.connect()
        assert s.connection_id > 0
        assert echo_once_reliable(s, marker_int(RELIABLE_MARKER) & 0xFFFF_FFFF, timeout=5.0) is not None
    assert s._tcp_sock is None
    assert s._rudp is None
