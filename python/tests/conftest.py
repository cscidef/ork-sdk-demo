"""pytest 共享 fixture：自动启停 demo 服务端 + 路径 / host / port 注入。

设计
----
- **repo root 定位**：`python/tests/conftest.py` → parents[1]=python、parents[2]=repo。
  把 `python/` 加入 sys.path，使测试可 `import lib.*`。
- **服务端生命周期**（session 级 `server` fixture）：
    · 若目标端口已被监听（例如已手动 `bash server/start.sh`），直接复用，测试结束
      **不** 关停它（谁启动谁负责）；
    · 否则调用 `server/start.sh`（GAME_PORT 环境变量指定端口）启动，轮询等待端口
      LISTEN + 一次 TCP 探测连通，yield，结束后 `server/stop.sh` 关停。
- **离线 vs 在线**：`test_frame_codec.py` / `test_cipher.py` 不请求 `server`，纯离线；
  其余在线测试通过 `server` fixture 触发自动启停。

环境变量
--------
- `ORK_TEST_HOST`（默认 127.0.0.1）
- `ORK_TEST_PORT`（默认 9400）
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

# ── 路径定位 ────────────────────────────────────────────────────────────────
TESTS_DIR = Path(__file__).resolve().parent
PYTHON_DIR = TESTS_DIR.parent            # .../python
REPO_ROOT = PYTHON_DIR.parent            # .../ork-sdk-demo
SERVER_DIR = REPO_ROOT / "server"

if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

HOST = os.environ.get("ORK_TEST_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORK_TEST_PORT", "9400"))


def _port_listening(host: str, port: int, timeout: float = 0.4) -> bool:
    """TCP 探测端口是否可连通（服务端 TCP bootstrap 监听同端口）。"""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _wait_port(host: str, port: int, timeout: float = 15.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _port_listening(host, port):
            return True
        time.sleep(0.2)
    return False


# ── 基础 fixture ─────────────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def host() -> str:
    return HOST


@pytest.fixture(scope="session")
def port() -> int:
    return PORT


@pytest.fixture(scope="session")
def python_dir() -> Path:
    return PYTHON_DIR


@pytest.fixture(scope="session")
def testdata_dir() -> Path:
    d = PYTHON_DIR / "testdata"
    assert d.is_dir(), f"testdata 目录缺失: {d}（先运行 python testdata/generate.py）"
    return d


@pytest.fixture(scope="session")
def server():
    """session 级：确保 demo 服务端在 (HOST, PORT) 就绪；按需启停。

    yield 一个 dict {host, port, started_by_fixture}。已在运行则复用、不关停。
    """
    if _port_listening(HOST, PORT):
        # 复用外部已启动的服务端（不负责关停）
        yield {"host": HOST, "port": PORT, "started_by_fixture": False}
        return

    start_sh = SERVER_DIR / "start.sh"
    stop_sh = SERVER_DIR / "stop.sh"
    assert start_sh.is_file(), f"未找到服务端启动脚本: {start_sh}"

    env = dict(os.environ, GAME_PORT=str(PORT))
    proc = subprocess.run(
        ["bash", str(start_sh)], env=env, cwd=str(SERVER_DIR),
        capture_output=True, text=True, timeout=60,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"服务端启动失败 (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}"
        )
    if not _wait_port(HOST, PORT, timeout=20.0):
        # 启动脚本报成功但端口未就绪 → 收尾并失败
        subprocess.run(["bash", str(stop_sh)], cwd=str(SERVER_DIR),
                       capture_output=True, text=True, timeout=30)
        raise RuntimeError(f"服务端端口 {HOST}:{PORT} 未在超时内就绪")

    try:
        yield {"host": HOST, "port": PORT, "started_by_fixture": True}
    finally:
        subprocess.run(["bash", str(stop_sh)], cwd=str(SERVER_DIR),
                       capture_output=True, text=True, timeout=30)
