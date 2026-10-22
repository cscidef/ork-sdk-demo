"""
拆分 proto 后的统一 import 面 — demo 客户端使用 `import pb_compat as pb`。

- 信封：NexusMessage_pb2（ClientMessage / ServerMessage）
- 会话：ork.session_pb2
- 玩法参考：reference.harmonia_gameplay_pb2
"""

from __future__ import annotations

import NexusMessage_pb2 as _envelope
import ork.session_pb2 as _session
import reference.harmonia_gameplay_pb2 as _gameplay

ClientMessage = _envelope.ClientMessage
ServerMessage = _envelope.ServerMessage

# 会话层 enum / message
from ork.session_pb2 import *  # noqa: F403

# Harmonia 玩法参考 message
from reference.harmonia_gameplay_pb2 import *  # noqa: F403

__all__ = [
    "ClientMessage",
    "ServerMessage",
    *_session.DESCRIPTOR.message_types_by_name.keys(),
    *_gameplay.DESCRIPTOR.message_types_by_name.keys(),
]
