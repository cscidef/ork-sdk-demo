"""Ork Python demo 传输库（精简版，全 opaque、零 protobuf 依赖）。

子模块（按需 import，避免在纯离线场景强依赖已编译的 KCP bridge）：
  - lib.ork_frame          Bootstrap V2(109B) / Handshake V2(42B) / 15B 帧头编解码
  - lib.ork_cipher         ChaCha20-Poly1305-IETF 传输密码层（seal/open + replay）
  - lib.ork_client_session OrkClientSession：单客户端完整会话（依赖 lib.kcp）
  - lib.kcp.ikcp_binding   RudpSession：ctypes 绑定 kcp_bridge 共享库

本 __init__ 故意保持轻量：`import lib.ork_frame` / `import lib.ork_cipher`
不会触发 KCP 共享库加载，因此离线单元测试无需先编译 bridge。
"""

from __future__ import annotations

__all__ = ["ork_frame", "ork_cipher", "ork_client_session", "kcp"]
