"""
Ork Cipher — 客户端包内 re-export。

实现以 python/ork_cipher.py 为准，避免双份拷贝漂移。
"""

from __future__ import annotations

import importlib.util
import os

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 显式加载根目录 ork_cipher.py，避免被 client.ork_cipher 包路径遮蔽
_spec = importlib.util.spec_from_file_location(
    "_ork_cipher_root", os.path.join(_ROOT, "ork_cipher.py")
)
_mod = importlib.util.module_from_spec(_spec)  # type: ignore[arg-type]
import sys as _sys
_sys.modules["_ork_cipher_root"] = _mod
_spec.loader.exec_module(_mod)  # type: ignore[union-attr]

CHANNEL_KCP = _mod.CHANNEL_KCP
CHANNEL_RUDP = CHANNEL_KCP  # 向后兼容别名
CHANNEL_UDP = _mod.CHANNEL_UDP
C2S = _mod.C2S
S2C = _mod.S2C
CIPHER_VERSION = _mod.CIPHER_VERSION
FRAME_HEADER_SIZE = _mod.FRAME_HEADER_SIZE
UDP_REPLAY_WINDOW = _mod.UDP_REPLAY_WINDOW
ChannelCipherState = _mod.ChannelCipherState
CipherState = _mod.CipherState
commit_udp_replay = _mod.commit_udp_replay
make_aad = _mod.make_aad
make_nonce = _mod.make_nonce
open_frame = _mod.open_frame
seal = _mod.seal
udp_cheap_filter = _mod.udp_cheap_filter

# Phase 3.5: frame codec re-exports
import importlib.util as _ilu2
import os as _os2
_frame_spec = _ilu2.spec_from_file_location(
    "_ork_frame_root",
    _os2.path.join(_os2.path.dirname(_os2.path.dirname(_os2.path.abspath(__file__))), "ork_frame.py"),
)
_frame_mod = _ilu2.module_from_spec(_frame_spec)
_sys.modules["_ork_frame_root"] = _frame_mod
_frame_spec.loader.exec_module(_frame_mod)

FrameType = _frame_mod.FrameType
ORK_FRAME_HEADER_SIZE = _frame_mod.ORK_FRAME_HEADER_SIZE
BOOTSTRAP_FRAME_SIZE = _frame_mod.BOOTSTRAP_FRAME_SIZE
encode_ork_frame_header = _frame_mod.encode_ork_frame_header
decode_ork_frame_header = _frame_mod.decode_ork_frame_header
encode_bootstrap_frame = _frame_mod.encode_bootstrap_frame
decode_bootstrap_frame = _frame_mod.decode_bootstrap_frame

# B1: Handshake V2 codec re-exports（42B 认证握手帧，Rev 3 §5.5/§5.6）
UDP_HANDSHAKE_V2_TYPE = _frame_mod.UDP_HANDSHAKE_V2_TYPE
UDP_HANDSHAKE_V2_VERSION = _frame_mod.UDP_HANDSHAKE_V2_VERSION
UDP_HANDSHAKE_V2_SIZE = _frame_mod.UDP_HANDSHAKE_V2_SIZE
handshake_counter_can_advance = _frame_mod.handshake_counter_can_advance
derive_handshake_key = _frame_mod.derive_handshake_key
build_handshake_v2_packet = _frame_mod.build_handshake_v2_packet
decode_handshake_v2_frame = _frame_mod.decode_handshake_v2_frame
verify_handshake_v2_auth_tag = _frame_mod.verify_handshake_v2_auth_tag

__all__ = [
    "CHANNEL_KCP",
    "CHANNEL_RUDP",
    "CHANNEL_UDP",
    "C2S",
    "CIPHER_VERSION",
    "FRAME_HEADER_SIZE",
    "S2C",
    "UDP_REPLAY_WINDOW",
    "ChannelCipherState",
    "CipherState",
    "commit_udp_replay",
    "make_aad",
    "make_nonce",
    "open_frame",
    "seal",
    "udp_cheap_filter",
    "FrameType",
    "ORK_FRAME_HEADER_SIZE",
    "BOOTSTRAP_FRAME_SIZE",
    "encode_ork_frame_header",
    "decode_ork_frame_header",
    "encode_bootstrap_frame",
    "decode_bootstrap_frame",
    "UDP_HANDSHAKE_V2_TYPE",
    "UDP_HANDSHAKE_V2_VERSION",
    "UDP_HANDSHAKE_V2_SIZE",
    "handshake_counter_can_advance",
    "derive_handshake_key",
    "build_handshake_v2_packet",
    "decode_handshake_v2_frame",
    "verify_handshake_v2_auth_tag",
]
