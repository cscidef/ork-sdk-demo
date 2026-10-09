"""ctypes 绑定 kcp_bridge.c，避免 Python 回调破坏 ikcp 内部状态。

注：C 代码使用 "KCP" 是因为底层库名，但概念上这是 RUDP（Reliable UDP）。
共享库需先用同目录 build.sh 编译（macOS → .dylib / Linux → .so）。
"""

from __future__ import annotations

import ctypes
import platform
from pathlib import Path

_sys = platform.system()
if _sys == "Darwin":
    _LIB_NAME = "libkcp_bridge.dylib"
elif _sys == "Windows":
    _LIB_NAME = "kcp_bridge.dll"
else:
    _LIB_NAME = "libkcp_bridge.so"
_LIB_PATH = Path(__file__).resolve().parent / _LIB_NAME

if not _LIB_PATH.exists():
    raise FileNotFoundError(
        f"未找到 {_LIB_PATH}，请先编译 KCP bridge: "
        f"cd python && bash lib/kcp/build.sh"
    )

_lib = ctypes.CDLL(str(_LIB_PATH))

_lib.kcp_bridge_create.argtypes = [ctypes.c_uint32]
_lib.kcp_bridge_create.restype = ctypes.c_int

_lib.kcp_bridge_release.argtypes = [ctypes.c_int]
_lib.kcp_bridge_release.restype = None

_lib.kcp_bridge_send.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
_lib.kcp_bridge_send.restype = ctypes.c_int

_lib.kcp_bridge_input.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
_lib.kcp_bridge_input.restype = None

_lib.kcp_bridge_update.argtypes = [ctypes.c_int, ctypes.c_uint32]
_lib.kcp_bridge_update.restype = None

_lib.kcp_bridge_recv.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
_lib.kcp_bridge_recv.restype = ctypes.c_int

_lib.kcp_bridge_peeksize.argtypes = [ctypes.c_int]
_lib.kcp_bridge_peeksize.restype = ctypes.c_int

_lib.kcp_bridge_poll_output.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
_lib.kcp_bridge_poll_output.restype = ctypes.c_int


class RudpSession:
    """与 Ork RudpSession 参数对齐的 Python 封装（概念上称为 RUDP，底层使用 KCP 库）。"""

    def __init__(self, conv: int, output_fn):
        self._output_fn = output_fn
        self._sid = _lib.kcp_bridge_create(conv)
        if self._sid < 0:
            raise RuntimeError(
                "kcp_bridge_create failed（并发 RUDP 会话已满；"
                "增大 kcp_bridge.c 的 KCP_BRIDGE_MAX_SESSIONS 后运行 lib/kcp/build.sh 重编）"
            )

    def _drain_output(self) -> None:
        buf = ctypes.create_string_buffer(2048)
        while True:
            n = _lib.kcp_bridge_poll_output(self._sid, buf, len(buf))
            if n <= 0:
                break
            self._output_fn(buf.raw[:n])

    def send(self, data: bytes) -> None:
        ret = _lib.kcp_bridge_send(self._sid, data, len(data))
        if ret < 0:
            raise RuntimeError(f"kcp_bridge_send failed: {ret}")
        self._drain_output()

    def input(self, data: bytes) -> None:
        _lib.kcp_bridge_input(self._sid, data, len(data))

    def recv(self) -> bytes | None:
        size = _lib.kcp_bridge_peeksize(self._sid)
        if size < 0:
            return None
        buf = ctypes.create_string_buffer(size)
        got = _lib.kcp_bridge_recv(self._sid, buf, size)
        if got < 0:
            return None
        return buf.raw[:got]

    def update(self, ms: int) -> None:
        _lib.kcp_bridge_update(self._sid, ms & 0xFFFFFFFF)
        self._drain_output()

    def close(self) -> None:
        if self._sid >= 0:
            _lib.kcp_bridge_release(self._sid)
            self._sid = -1

    def __del__(self):
        self.close()
