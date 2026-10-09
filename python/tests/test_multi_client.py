"""在线测试 — 3 个客户端并发 echo。

每客户端独立会话（各自 connectionId / UDP socket / KCP 会话），在独立线程内
连接并做若干次 Reliable echo；验证全部连接成功且 echo 全部送达。
"""

from __future__ import annotations

import threading

from lib.demo_helpers import (
    RELIABLE_MARKER,
    echo_once_reliable,
    marker_int,
)
from lib.ork_client_session import OrkClientSession

NUM_CLIENTS = 3
ECHOES_PER_CLIENT = 3


def _run_client(host: str, port: int, index: int, results: list[dict]) -> None:
    res = {"index": index, "connected": False, "conn_id": 0, "ok": 0, "sent": 0, "error": ""}
    s: OrkClientSession | None = None
    try:
        s = OrkClientSession(host, port, f"test-multi#{index}")
        s.connect()
        res["connected"] = True
        res["conn_id"] = s.connection_id
        base = (marker_int(RELIABLE_MARKER) & 0xFFFF) * 1000 + index * 100_000
        for k in range(ECHOES_PER_CLIENT):
            res["sent"] += 1
            if echo_once_reliable(s, base + k, timeout=5.0) is not None:
                res["ok"] += 1
    except Exception as exc:  # noqa: BLE001
        res["error"] = str(exc)
    finally:
        if s is not None:
            s.close()
        results[index] = res


def test_three_concurrent_clients(server, host, port):
    results: list[dict] = [{} for _ in range(NUM_CLIENTS)]
    threads = [
        threading.Thread(target=_run_client, args=(host, port, i, results), daemon=True)
        for i in range(NUM_CLIENTS)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=60)

    connected = sum(1 for r in results if r.get("connected"))
    total_sent = sum(r.get("sent", 0) for r in results)
    total_ok = sum(r.get("ok", 0) for r in results)

    assert connected == NUM_CLIENTS, f"仅 {connected}/{NUM_CLIENTS} 客户端连接成功: {results}"
    assert total_sent == NUM_CLIENTS * ECHOES_PER_CLIENT
    assert total_ok == total_sent, f"echo 未全部送达 ({total_ok}/{total_sent}): {results}"

    # connectionId 应互不相同（每客户端独立分配）
    ids = [r["conn_id"] for r in results]
    assert len(set(ids)) == NUM_CLIENTS, f"connectionId 应唯一: {ids}"
