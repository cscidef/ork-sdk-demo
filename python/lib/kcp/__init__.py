"""KCP bridge 包：ctypes 绑定 kcp_bridge.c（编译为共享库）。

概念上这是 RUDP（Reliable UDP）；C 代码沿用底层库名 "KCP"。
"""

from __future__ import annotations

__all__ = ["ikcp_binding"]
