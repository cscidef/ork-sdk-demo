"""Ork 客户端会话封装 — 供 smoke test 与 Unity 模拟压测复用。"""

from __future__ import annotations

import math
import os
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

# 确保可以从 python/ 找到 kcp/、generated/ 等同级包
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GEN = Path(__file__).resolve().parent.parent / "generated"
sys.path.insert(0, str(GEN))

import pb_compat as pb  # noqa: E402

from lib.kcp.ikcp_binding import RudpSession  # noqa: E402

from client.ork_cipher import (
    CipherState,
    seal,
    open_frame,
    C2S,
    S2C,
    CHANNEL_RUDP,
    CHANNEL_UDP,
    FrameType,
    decode_bootstrap_frame,
    BOOTSTRAP_FRAME_SIZE,
    build_handshake_v2_packet,
    handshake_counter_can_advance,
    UDP_HANDSHAKE_V2_SIZE,
    UDP_HANDSHAKE_V2_TYPE,
)

# Phase 6.2: 应用层弱网损伤注入（可选）。impairment.py 与本文件同包，
# 直接导入不会引入循环依赖；为了保持"不挂 engine 就零开销"，只在
# __init__ 里存引用，收发路径用 `is None` 短路。
from client.impairment import ImpairmentEngine

UDP_HANDSHAKE_V1 = 0  # 旧 9B 形态：B1 硬切后服务端静默拒绝（仅拒绝路径测试使用）
UDP_HANDSHAKE_V2 = 3  # B1: Handshake V2（42B，Rev 3 §5.5）
UDP_DATA = 2

# UDP 数据包头格式（与服务端 UdpTransportServer 一致）
# type(1B) + connectionId(4B BE) + payloadSize(2B BE) = 7B
UDP_DATA_HEADER_SIZE = 7

# Unity Humanoid 常见骨骼名（精简集）
UNITY_BONE_NAMES = (
    "Hips",
    "Spine",
    "Chest",
    "Neck",
    "Head",
    "LeftUpperArm",
    "LeftLowerArm",
    "LeftHand",
    "RightUpperArm",
    "RightLowerArm",
    "RightHand",
    "LeftUpperLeg",
    "LeftLowerLeg",
    "LeftFoot",
    "RightUpperLeg",
    "RightLowerLeg",
    "RightFoot",
)

HUMANOID_BONES = UNITY_BONE_NAMES

try:
    from google.protobuf import text_format
except ImportError:
    text_format = None  # type: ignore[assignment]


def read_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("TCP 连接已关闭")
        buf += chunk
    return buf


def read_tcp_length_prefixed(sock: socket.socket, *, expected_length: int | None = None) -> bytes:
    """读取 4 字节大端长度前缀 + payload（Bootstrap V2 硬切）。

    - `length` 先做合理上限校验（≤ 4 MiB），防畸形长度导致超量读取；
    - `expected_length` 给定时，声明的 length 必须严格相等（外层 prefix
      冻结语义：Bootstrap V2 必须等于 109，且不含 prefix 自身 4 字节）。
    """
    (length,) = struct.unpack("!I", read_exact(sock, 4))
    if length == 0 or length > 4 * 1024 * 1024:
        raise ValueError(f"非法 TCP 消息长度: {length}")
    if expected_length is not None and length != expected_length:
        raise ValueError(
            f"非法 TCP 消息长度: {length}（期望 {expected_length}）"
        )
    return read_exact(sock, length)


# 兼容旧名
read_tcp_protobuf = read_tcp_length_prefixed


def send_tcp_protobuf(sock: socket.socket, payload: bytes) -> int:
    frame = struct.pack("!I", len(payload)) + payload
    sock.sendall(frame)
    return len(frame)


def parse_server_message(data: bytes) -> pb.ServerMessage:
    msg = pb.ServerMessage()
    msg.ParseFromString(data)
    return msg


def safe_parse_server_message(data: bytes) -> pb.ServerMessage | None:
    """尝试解析为 ServerMessage；失败时返回 None（opaque payload 场景）。"""
    try:
        msg = pb.ServerMessage()
        msg.ParseFromString(data)
        # protobuf 对无效输入可能不抛异常但字段为空
        return msg
    except Exception:
        return None


def build_join_message(player_uuid: str) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    msg.join.player_uuid = player_uuid
    return msg.SerializeToString()


def build_leave_message(player_uuid: str) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    msg.leave.player_uuid = player_uuid
    return msg.SerializeToString()


def build_client_ack(connection_id: int, last_ack_step: int) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    msg.ack.connection_id = connection_id
    msg.ack.last_ack_step = last_ack_step
    return msg.SerializeToString()


def build_bone_transform(
    bone_name: str,
    local_pos: tuple[float, float, float],
    local_rot: tuple[float, float, float, float],
    blend_weight: float,
) -> pb.BoneTransform:
    bone = pb.BoneTransform()
    bone.bone_name = bone_name
    bone.local_position.extend(local_pos)
    bone.local_rotation.extend(local_rot)
    bone.blend_weight = blend_weight
    return bone


def build_rich_sync_pose_message(
    player_id: int,
    position: tuple[float, float, float],
    rotation: tuple[float, float, float, float],
    timestamp_ms: int,
    *,
    velocity: tuple[float, float, float] | None = None,
    angular_velocity: tuple[float, float, float] | None = None,
    animation_state_id: int = 0,
    bone_count: int = 0,
    device_metadata: dict[str, str] | None = None,
) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    pose = msg.sync_pose
    pose.player_id = player_id
    pose.position.extend(position)
    pose.rotation.extend(rotation)
    pose.timestamp = timestamp_ms

    if velocity:
        pose.velocity.extend(velocity)
    if angular_velocity:
        pose.angular_velocity.extend(angular_velocity)
    if animation_state_id:
        pose.animation_state_id = animation_state_id

    for i in range(min(bone_count, len(UNITY_BONE_NAMES))):
        angle = (i + 1) * 0.15
        bone = build_bone_transform(
            UNITY_BONE_NAMES[i],
            (math.sin(angle) * 0.05, i * 0.02, math.cos(angle) * 0.05),
            (0.0, math.sin(angle * 0.5), 0.0, math.cos(angle * 0.5)),
            1.0 - i * 0.02,
        )
        pose.bones.append(bone)

    if device_metadata:
        pose.device_metadata.update(device_metadata)

    return msg.SerializeToString()


# ---------------------------------------------------------------------------
# Phase 4.0: ORKBENCH1 opaque benchmark ping
# 帧格式：prefix(9B "ORKBENCH1") + seq(8B LE) + send_time_ns(8B LE) = 25B
# ---------------------------------------------------------------------------

ORKBENCH_PREFIX = b"ORKBENCH1"
ORKBENCH_SIZE = 25  # 9 + 8 + 8


def build_bench_payload(sequence: int, send_time_ns: int) -> bytes:
    """构建 ORKBENCH1 benchmark ping payload（25 字节）。"""
    return ORKBENCH_PREFIX + struct.pack("<QQ", sequence, send_time_ns)


def parse_bench_payload(data: bytes) -> tuple[int, int] | None:
    """解析 ORKBENCH1 payload → (sequence, send_time_ns)；不匹配返回 None。"""
    if len(data) != ORKBENCH_SIZE or data[:9] != ORKBENCH_PREFIX:
        return None
    seq, ts = struct.unpack("<QQ", data[9:25])
    return seq, ts


def build_reliable_gameplay_message(
    player_id: int,
    command_seq: int,
    *,
    interaction_target: int | None = None,
    interaction_type: str | None = None,
    equipment_slots: list[tuple[int, int, int]] | None = None,
    chat_message: str | None = None,
    timestamp_ms: int | None = None,
) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    cmd = msg.reliable_cmd
    cmd.player_id = player_id
    cmd.command_seq = command_seq
    cmd.client_timestamp = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)

    if interaction_target is not None and interaction_type:
        cmd.interaction.target_entity_id = interaction_target
        cmd.interaction.interaction_type = interaction_type
        cmd.interaction.hit_point.extend([0.0, 1.0, 0.0])
        cmd.interaction.hit_normal.extend([0.0, 1.0, 0.0])
    elif equipment_slots is not None:
        for slot_id, item_id, durability in equipment_slots:
            slot = cmd.equipment.slots.add()
            slot.slot_id = slot_id
            slot.item_id = item_id
            slot.durability = durability
        cmd.equipment.consumable_counts["potion"] = 3
        cmd.equipment.consumable_counts["ammo"] = 120
    elif chat_message is not None:
        cmd.chat_message = chat_message

    return msg.SerializeToString()


def build_entity_batch_message(
    sender_player_id: int,
    entities: list[tuple[int, tuple[float, float, float], tuple[float, float, float, float], int]],
    batch_timestamp_ms: int | None = None,
) -> bytes:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    batch = msg.entity_batch
    batch.sender_player_id = sender_player_id
    batch.batch_timestamp = batch_timestamp_ms if batch_timestamp_ms is not None else int(time.time() * 1000)

    for entity_id, pos, rot, ts in entities:
        ent = batch.entities.add()
        ent.entity_id = entity_id
        ent.position.extend(pos)
        ent.rotation.extend(rot)
        ent.timestamp = ts

    return msg.SerializeToString()


# ---------------------------------------------------------------------------
# ClientMessage 对象构建（供分步演示 / proto 预览）
# ---------------------------------------------------------------------------


def _client_msg() -> pb.ClientMessage:
    msg = pb.ClientMessage()
    msg.type = pb.CLIENT_COMMAND
    return msg


def format_client_message(msg: pb.ClientMessage) -> str:
    if text_format is not None:
        return text_format.MessageToString(msg, as_utf8=True)
    return str(msg)


def describe_payload_summary(msg: pb.ClientMessage) -> str:
    which = msg.WhichOneof("payload")
    if which is None:
        return "payload=(empty)"
    if which == "join":
        return f"JoinRequest {{ player_uuid: \"{msg.join.player_uuid}\" }}"
    if which == "leave":
        return f"LeaveRequest {{ player_uuid: \"{msg.leave.player_uuid}\" }}"
    if which == "ack":
        return (
            f"ClientAck {{ connection_id: {msg.ack.connection_id}, "
            f"last_ack_step: {msg.ack.last_ack_step} }}"
        )
    if which == "sync_pose":
        p = msg.sync_pose
        return (
            f"SyncPoseCommand {{ player_id: {p.player_id}, bones: {len(p.bones)}, "
            f"anim: {p.animation_state_id}, metadata: {len(p.device_metadata)}, "
            f"bytes≈{msg.ByteSize()} }}"
        )
    if which == "reliable_cmd":
        c = msg.reliable_cmd
        inner = c.WhichOneof("payload")
        return (
            f"ReliableGameplayCommand {{ player_id: {c.player_id}, seq: {c.command_seq}, "
            f"payload: {inner}, bytes≈{msg.ByteSize()} }}"
        )
    if which == "entity_batch":
        b = msg.entity_batch
        return (
            f"EntityStateBatch {{ sender: {b.sender_player_id}, "
            f"entities: {len(b.entities)}, bytes≈{msg.ByteSize()} }}"
        )
    return f"payload={which}"


def make_join(player_uuid: str) -> pb.ClientMessage:
    msg = _client_msg()
    msg.join.player_uuid = player_uuid
    return msg


def make_leave(player_uuid: str) -> pb.ClientMessage:
    msg = _client_msg()
    msg.leave.player_uuid = player_uuid
    return msg


def make_client_ack(connection_id: int, last_ack_step: int) -> pb.ClientMessage:
    msg = _client_msg()
    msg.ack.connection_id = connection_id
    msg.ack.last_ack_step = last_ack_step
    return msg


def make_bone(
    name: str,
    local_pos: tuple[float, float, float],
    local_rot: tuple[float, float, float, float],
    blend_weight: float = 1.0,
) -> pb.BoneTransform:
    bone = pb.BoneTransform()
    bone.bone_name = name
    bone.local_position.extend(local_pos)
    bone.local_rotation.extend(local_rot)
    bone.blend_weight = blend_weight
    return bone


def make_sync_pose(
    player_id: int,
    *,
    position: tuple[float, float, float] = (0.0, 1.6, 0.0),
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0),
    velocity: tuple[float, float, float] = (0.0, 0.0, 0.0),
    angular_velocity: tuple[float, float, float] = (0.0, 0.0, 0.0),
    animation_state_id: int = 0,
    bone_count: int = 0,
    device_metadata: dict[str, str] | None = None,
    timestamp_ms: int | None = None,
) -> pb.ClientMessage:
    msg = _client_msg()
    pose = msg.sync_pose
    pose.player_id = player_id
    pose.position.extend(position)
    pose.rotation.extend(rotation)
    pose.timestamp = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)
    pose.velocity.extend(velocity)
    pose.angular_velocity.extend(angular_velocity)
    pose.animation_state_id = animation_state_id

    for i in range(min(bone_count, len(UNITY_BONE_NAMES))):
        angle = (i + 1) * 0.2
        pose.bones.append(
            make_bone(
                UNITY_BONE_NAMES[i],
                (math.sin(angle) * 0.08, i * 0.025, math.cos(angle) * 0.08),
                (0.0, math.sin(angle * 0.5), 0.0, math.cos(angle * 0.5)),
                max(0.1, 1.0 - i * 0.03),
            )
        )

    meta = device_metadata or {
        "client": "unity",
        "engine": "Unity2022.3",
        "platform": "StandaloneOSX",
        "device_model": "Mac15,3",
        "app_version": "1.0.0-demo",
    }
    pose.device_metadata.update(meta)
    return msg


def make_interaction_command(
    player_id: int,
    command_seq: int,
    *,
    target_entity_id: int = 1001,
    interaction_type: str = "activate",
    hit_point: tuple[float, float, float] = (1.2, 0.5, 3.4),
    hit_normal: tuple[float, float, float] = (0.0, 1.0, 0.0),
    timestamp_ms: int | None = None,
) -> pb.ClientMessage:
    msg = _client_msg()
    cmd = msg.reliable_cmd
    cmd.player_id = player_id
    cmd.command_seq = command_seq
    cmd.client_timestamp = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)
    cmd.interaction.target_entity_id = target_entity_id
    cmd.interaction.interaction_type = interaction_type
    cmd.interaction.hit_point.extend(hit_point)
    cmd.interaction.hit_normal.extend(hit_normal)
    return msg


def make_equipment_command(
    player_id: int,
    command_seq: int,
    *,
    slots: list[tuple[int, int, int]] | None = None,
    consumables: dict[str, int] | None = None,
    timestamp_ms: int | None = None,
) -> pb.ClientMessage:
    msg = _client_msg()
    cmd = msg.reliable_cmd
    cmd.player_id = player_id
    cmd.command_seq = command_seq
    cmd.client_timestamp = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)

    slot_data = slots or [
        (0, 1001, 95),
        (1, 2002, 80),
        (2, 3003, 100),
        (3, 4004, 60),
    ]
    for slot_id, item_id, durability in slot_data:
        s = cmd.equipment.slots.add()
        s.slot_id = slot_id
        s.item_id = item_id
        s.durability = durability

    counts = consumables or {"potion_hp": 5, "potion_mp": 3, "ammo_rifle": 120, "grenade": 2}
    cmd.equipment.consumable_counts.update(counts)
    return msg


def make_chat_command(
    player_id: int,
    command_seq: int,
    text: str,
    *,
    timestamp_ms: int | None = None,
) -> pb.ClientMessage:
    msg = _client_msg()
    cmd = msg.reliable_cmd
    cmd.player_id = player_id
    cmd.command_seq = command_seq
    cmd.client_timestamp = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)
    cmd.chat_message = text
    return msg


def make_entity_batch(
    sender_player_id: int,
    entity_count: int,
    *,
    base_entity_id: int = 8000,
    batch_timestamp_ms: int | None = None,
) -> pb.ClientMessage:
    msg = _client_msg()
    batch = msg.entity_batch
    batch.sender_player_id = sender_player_id
    batch.batch_timestamp = batch_timestamp_ms if batch_timestamp_ms is not None else int(time.time() * 1000)

    now = batch.batch_timestamp
    for i in range(entity_count):
        angle = i * 0.55
        ent = batch.entities.add()
        ent.entity_id = base_entity_id + i
        ent.position.extend([
            math.cos(angle) * (4.0 + i * 0.3),
            0.2 + (i % 3) * 0.5,
            math.sin(angle) * (4.0 + i * 0.3),
        ])
        half = angle * 0.5
        ent.rotation.extend([0.0, math.sin(half), 0.0, math.cos(half)])
        ent.timestamp = now + i
    return msg


def preview_all_client_messages(player_id: int = 1, connection_id: int = 999, uuid: str = "proto-preview") -> None:
    """打印 proto 中全部 ClientMessage 示例（无需连服务器）。"""
    specs = {
        "JoinRequest": make_join(uuid),
        "LeaveRequest": make_leave(uuid),
        "ClientAck": make_client_ack(connection_id, 42),
        "SyncPoseCommand": make_sync_pose(player_id, bone_count=len(UNITY_BONE_NAMES), animation_state_id=12345),
        "ReliableGameplay/Interaction": make_interaction_command(player_id, 1),
        "ReliableGameplay/Equipment": make_equipment_command(player_id, 2),
        "ReliableGameplay/Chat": make_chat_command(player_id, 3, f"[team] player {player_id} ready"),
        "EntityStateBatch": make_entity_batch(player_id, entity_count=8),
    }
    print("NexusMessage.proto — 客户端消息示例\n")
    for name, msg in specs.items():
        print(f"======== {name} ({msg.ByteSize()} bytes) ========")
        print(format_client_message(msg))
        print()


def wrap_udp_data(connection_id: int, payload: bytes) -> bytes:
    header = struct.pack("!BIH", UDP_DATA, connection_id, len(payload))
    return header + payload


def wrap_udp_handshake(
    connection_id: int,
    client_nonce: bytes,
    counter: int,
    bootstrap_epoch: bytes,
    c2s_key: bytes,
) -> bytes:
    """构造 42B Handshake V2 握手包（B1；Rev 3 §5.5/§5.6）。

    handshakeKey 由 c2sKey 经 HKDF-SHA-256 派生（info label
    "ORK-UDP-HS-C2S-v2"），不复用数据通道密钥；authTag 覆盖除自身
    外全部 26B 明文字段。旧 9B V1（type=0 + connectionId +
    sessionToken）已硬切删除，不再提供编码路径。
    """
    return build_handshake_v2_packet(
        connection_id, client_nonce, counter, bootstrap_epoch, c2s_key
    )


def is_ork_framed_udp(packet: bytes) -> bool:
    """区分 Ork UDP 定长帧与 KCP 裸包（conv 首字节可能与枚举碰撞）。"""
    if not packet:
        return False
    kind = packet[0]
    if kind == UDP_HANDSHAKE_V2:
        # B1: V2 为唯一握手帧形态（exact 42B）；9B V1 不再属于 Ork 帧
        return len(packet) == UDP_HANDSHAKE_V2_SIZE
    if kind == UDP_DATA:
        if len(packet) < 7:
            return False
        _conn_id, payload_len = struct.unpack("!IH", packet[1:7])
        return len(packet) == 7 + payload_len
    return False


def describe_server_message(msg: pb.ServerMessage) -> str:
    which = msg.WhichOneof("payload")
    if which == "assign":
        # A0.1: 不输出 session_token 值（凭证材料）；connectionId 可记录。
        return f"AssignConnection id={msg.assign.connection_id}"
    if which == "ack":
        return f"UdpAck: {msg.ack.message}"
    if which == "assign_player":
        return (
            f"AssignPlayerId conn={msg.assign_player.connection_id} "
            f"player={msg.assign_player.player_id}"
        )
    if which == "snapshot":
        return f"FullSnapshot step={msg.snapshot.current_step} players={len(msg.snapshot.players)}"
    if which == "delta":
        return f"NetDelta {msg.delta.from_step}->{msg.delta.to_step}"
    return f"type={msg.type} payload={which}"


@dataclass
class SessionStats:
    tcp_bytes_rx: int = 0
    tcp_bytes_tx: int = 0
    tcp_probes_sent: int = 0
    rudp_bytes_tx: int = 0  # 概念上称为 RUDP
    poses_sent: int = 0
    pose_bytes: int = 0
    acks_sent: int = 0
    ack_bytes: int = 0
    reliable_cmds_sent: int = 0
    reliable_bytes: int = 0
    entity_batches_sent: int = 0
    entity_batch_bytes: int = 0
    rudp_received: int = 0  # 概念上称为 RUDP（含 UDP 不可靠收包）
    udp_received: int = 0   # UDP 不可靠通道收包计数
    udp_rtt_ms: list[float] = field(default_factory=list)  # UDP RTT 采样
    snapshots: int = 0
    deltas: int = 0
    errors: list[str] = field(default_factory=list)


class OrkClientSession:
    """单客户端完整会话：TCP bootstrap → UDP/KCP → Join → 三通道收发。"""

    def __init__(
        self,
        host: str,
        port: int,
        player_uuid: str,
        *,
        connect_timeout: float = 5.0,
        join_timeout: float = 20.0,
        impairment: "ImpairmentEngine | None" = None,
    ):
        self.host = host
        self.port = port
        self.player_uuid = player_uuid
        self.connect_timeout = connect_timeout
        self.join_timeout = join_timeout
        # Phase 6.2: 可选的应用层损伤引擎；None = 透传（默认，不影响既有测试）
        self._impairment = impairment

        self.connection_id = 0
        self.session_token = 0
        # A0.3: Bootstrap V2 安全域标识（16B，客户端状态保存；不写日志）
        self.bootstrap_epoch = b""
        # B1: Handshake V2 客户端状态（Rev 3 §5.8）——nonce 为 16B CSPRNG，
        # counter 从 0 起、重试同 nonce 严格递增；c2sKey/handshakeKey 仅驻留
        # 内存，不日志、不序列化。
        self._hs_client_nonce: bytes | None = None
        self._hs_counter: int = 0
        self._hs_c2s_key: bytes = b""
        self.player_id = 0
        self.cipher: CipherState | None = None

        self._tcp_sock: socket.socket | None = None
        self._udp_sock: socket.socket | None = None
        self._rudp: RudpSession | None = None  # 概念上称为 RUDP（Reliable UDP）
        self._stop = threading.Event()
        self._udp_thread: threading.Thread | None = None
        self._clock_base = 0.0
        self.stats = SessionStats()
        self._udp_rx_payloads: list[bytes] = []  # UDP 不可靠通道收到的原始载荷
        self._bench_rudp_rx: list[bytes] = []  # Phase 4.0: ORKBENCH1 RUDP 回显
        self._bench_udp_rx: list[bytes] = []   # Phase 4.0: ORKBENCH1 UDP 回显
        self._udp_ack_received = False  # Phase 3.5: 文本 ACK 标记

    def _server_udp(self) -> tuple[str, int]:
        return (self.host, self.port)

    def _wire_sendto(self, data: bytes, addr: tuple[str, int]) -> bool:
        """统一的 UDP 发送出口（Phase 6.2）。

        未挂损伤引擎时等价于 `sock.sendto(data, addr)`；挂了则经过
        `impair_send`（loss / bandwidth / delay / dup）。返回是否实际上线。

        把损伤放在**线缆层**（而不是 KCP 之上）是为了让 KCP 的 ARQ 能
        看到丢包并重传 —— 门禁要求"loss=5% 时 KCP 送达率仍 ~100%、UDP
        ≈95%"，只有线缆层注入才能测出这个差异。
        """
        sock = self._udp_sock
        if sock is None:
            return False
        if self._impairment is None:
            sock.sendto(data, addr)
            return True
        return self._impairment.impair_send(data, lambda b: sock.sendto(b, addr))

    def rudp_now_ms(self) -> int:
        """返回 RUDP 时钟（概念上称为 RUDP，底层使用 KCP 库）。"""
        return int((time.monotonic() - self._clock_base) * 1000)

    def connect_tcp_bootstrap(self, *, tcp_probe: bool = False) -> str:
        """步骤 1：TCP 连接并接收 Bootstrap V2 帧（109B opaque，硬切单轨）。"""
        tcp_addr = (self.host, self.port)
        self._tcp_sock = socket.create_connection(tcp_addr, timeout=self.connect_timeout)
        self._tcp_sock.settimeout(self.connect_timeout)

        # Bootstrap V2 硬切：外层 4B 大端 length-prefix 必须严格 = 109
        # （不含 prefix 自身）；不猜 V1、不接受 81B。
        raw = read_tcp_length_prefixed(
            self._tcp_sock, expected_length=BOOTSTRAP_FRAME_SIZE
        )
        self.stats.tcp_bytes_rx += 4 + len(raw)

        if len(raw) != BOOTSTRAP_FRAME_SIZE:
            raise RuntimeError(
                f"BootstrapFrame 长度非法: {len(raw)} != {BOOTSTRAP_FRAME_SIZE}"
            )

        bootstrap = decode_bootstrap_frame(raw)
        self.connection_id = bootstrap["connection_id"]
        self.session_token = bootstrap["session_token"]
        self.bootstrap_epoch = bootstrap["bootstrap_epoch"]

        # B1: 每次成功解析 Bootstrap V2 后生成新的 16B ClientNonce（CSPRNG），
        # counter 从 0 开始（Rev 3 §5.8）；后续重试同 nonce 严格递增。
        self._hs_client_nonce = os.urandom(16)
        self._hs_counter = 0
        self._hs_c2s_key = bootstrap["c2s_key"]

        # 初始化 cipher（明文模式也需要，用于帧头解析 + replay 跟踪）
        self.cipher = CipherState.from_bootstrap(bootstrap)

        enc_label = "encrypted" if bootstrap["encryption_required"] else "plaintext"
        # A0.1: summary 不得包含 session_token 值（凭证材料）；connectionId 可记录。
        summary = f"BootstrapFrame conn={self.connection_id} {enc_label}"

        if tcp_probe:
            probe = build_join_message(f"{self.player_uuid}-tcp-probe")
            self.stats.tcp_bytes_tx += send_tcp_protobuf(self._tcp_sock, probe)
            self.stats.tcp_probes_sent += 1

        return summary

    def start_udp_rudp(self) -> None:
        """步骤 2 准备：绑定 UDP 并创建 RUDP 会话（概念称呼，底层使用 KCP 库）。"""
        self._udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp_sock.bind(("0.0.0.0", 0))
        self._udp_sock.settimeout(0.02)

        def rudp_output(data: bytes) -> None:
            # Phase 6.2: KCP 的分片输出也走统一线缆出口，让损伤引擎能看到
            # 真实上线的 UDP 包（从而 KCP ARQ 可以对丢包做出反应）。
            self._wire_sendto(data, self._server_udp())

        self._rudp = RudpSession(self.connection_id, rudp_output)
        self._clock_base = time.monotonic()

        self._udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        self._udp_thread.start()

    def start_udp_kcp(self) -> None:
        """兼容旧脚本名；等价于 start_udp_rudp()。"""
        self.start_udp_rudp()

    def send_udp_handshake(self) -> None:
        """步骤 2：UDP Handshake V2（42B 认证帧；B1）。

        同一 bootstrap 会话内 nonce 固定、counter 单调递增（Rev 3 §5.8）；
        counter 溢出 fail closed（须重建连接，不回绕）。
        """
        assert self._udp_sock is not None
        if self._hs_client_nonce is None or not self._hs_c2s_key:
            raise RuntimeError("Handshake V2 状态缺失（bootstrap 未完成）")
        if not handshake_counter_can_advance(self._hs_counter):
            raise RuntimeError("handshake counter exhausted (fail closed; reconnect required)")
        self._wire_sendto(
            wrap_udp_handshake(
                self.connection_id,
                self._hs_client_nonce,
                self._hs_counter,
                self.bootstrap_epoch,
                self._hs_c2s_key,
            ),
            self._server_udp(),
        )

    def wait_udp_ack(self, timeout: float = 5.0, *, retry_interval: float = 0.5) -> str:
        """步骤 3：等待 KCP UdpAck（B1 握手重试：同 nonce、counter 严格递增）。

        每 retry_interval 秒未收到 ACK 则重发握手帧（counter +1）；
        服务端幂等：首次绑定发 ACK，重试帧仅重发 ACK 不重复 Connected。
        """
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("未收到 UdpAck")
            if self._wait_for_udp_ack(timeout=min(retry_interval, remaining)):
                return "ORK-UDP-ACK:handshake"
            # 超时未收到 ACK → 同 nonce 重发，counter 严格递增（Rev 3 §5.8）
            if not handshake_counter_can_advance(self._hs_counter):
                raise RuntimeError(
                    "handshake counter exhausted (fail closed; reconnect required)"
                )
            self._hs_counter += 1
            self.send_udp_handshake()

    def send_join_and_wait(self, timeout: float = 5.0) -> int:
        """步骤 4：RUDP Join 并等待 AssignPlayerId。"""
        self.send_rudp(build_join_message(self.player_uuid))
        if not self._wait_for_player_id(timeout=timeout):
            raise TimeoutError("未收到 AssignPlayerId")
        return self.player_id

    def connect_and_join(self, *, tcp_probe: bool = False, join_timeout: float | None = None) -> None:
        timeout = self.join_timeout if join_timeout is None else join_timeout
        self.connect_tcp_bootstrap(tcp_probe=tcp_probe)
        self.start_udp_rudp()
        self.send_udp_handshake()
        self.wait_udp_ack(timeout=timeout)
        self.send_join_and_wait(timeout=timeout)

    def _udp_loop(self) -> None:
        assert self._udp_sock is not None
        assert self._rudp is not None
        while not self._stop.is_set():
            try:
                packet, _addr = self._udp_sock.recvfrom(8192)
            except socket.timeout:
                continue
            except ConnectionResetError:
                # Windows: ICMP port unreachable — 忽略并继续
                continue
            except OSError:
                break
            # Phase 6.2: 接收侧损伤（loss + reorder，不加 delay）。
            #   - 返回 None：本轮不交付（丢包或被 reorder 暂存）。
            #   - 返回 bytes：可能是当前包，也可能是之前暂存的旧包（乱序）。
            # 放在 KCP input 之前，让 KCP 看到真实的丢包/乱序并自己恢复。
            if self._impairment is not None:
                impaired = self._impairment.impair_recv(packet)
                if impaired is None:
                    continue
                packet = impaired
            if self._is_udp_data_packet(packet):
                # 服务端 UdpDataHeader 包裹的 OrkFrame：解帧并计收包
                self._handle_udp_data_packet(packet)
                continue
            # 其他：原始 KCP 包
            try:
                self._rudp.input(packet)
            except RuntimeError as exc:
                self.stats.errors.append(f"RUDP input: {exc}")

    def _is_udp_data_packet(self, packet: bytes) -> bool:
        """检测服务端 UdpDataHeader 包裹的 UDP 包（type=0x02）。

        KCP 包首部 4B 是 conv=connection_id (LE)，当 connection_id 低位为 0x02
        会与 UDP_DATA type 碰撞。用长度交叉验证排除误判。
        """
        if len(packet) < UDP_DATA_HEADER_SIZE:
            return False
        if packet[0] != UDP_DATA:
            return False
        # 从 UdpDataHeader 解析 payload_size (BE) 并交叉验证包长
        _conn_id, payload_size = struct.unpack("!IH", packet[1:UDP_DATA_HEADER_SIZE])
        if _conn_id == 0:
            return False  # connection_id 不应为 0
        return len(packet) == UDP_DATA_HEADER_SIZE + payload_size

    def _handle_udp_data_packet(self, packet: bytes) -> None:
        """解包 UdpDataHeader → OrkFrame → 解密 → 记录统计。

        服务端 UDP 不可靠通道发送的数据，包含 7B UdpDataHeader。
        """
        if len(packet) < UDP_DATA_HEADER_SIZE:
            return
        _conn_id, payload_size = struct.unpack("!IH", packet[1:UDP_DATA_HEADER_SIZE])
        if len(packet) != UDP_DATA_HEADER_SIZE + payload_size:
            self.stats.errors.append("UDP data packet length mismatch")
            return
        ork_frame = packet[UDP_DATA_HEADER_SIZE:]
        if self.cipher is not None:
            payload = open_frame(self.cipher, CHANNEL_UDP, S2C, 0, ork_frame)
            if payload is None:
                self.stats.errors.append("UDP OrkFrame decrypt/open failed")
                return
        else:
            payload = ork_frame
        self.stats.udp_received += 1
        # Phase 4.0: ORKBENCH1 UDP 回显单独入队
        if parse_bench_payload(payload) is not None:
            self._bench_udp_rx.append(payload)
            return
        # 将 payload 存入一个简单队列，供上层读取（如 RTT 计数）
        self._udp_rx_payloads.append(payload)

    def pump_rudp(self) -> list[pb.ServerMessage]:
        """从 RUDP 通道收取消息（概念称呼，底层使用 KCP 库）。"""
        assert self._rudp is not None
        self._rudp.update(self.rudp_now_ms())
        messages: list[pb.ServerMessage] = []
        while True:
            payload = self._rudp.recv()
            if payload is None:
                break
            if self.cipher is not None:
                payload = open_frame(self.cipher, CHANNEL_RUDP, S2C, 0, payload)
                if payload is None:
                    self.stats.errors.append("RUDP decrypt/open failed")
                    continue
            # Phase 4.0: 检测 ORKBENCH1 回显（在 protobuf parse 之前）
            if parse_bench_payload(payload) is not None:
                self._bench_rudp_rx.append(payload)
                continue
            # Phase 3.5: 尝试解析为 protobuf；服务端可能发 opaque 文本（如 UDP ACK）
            msg = safe_parse_server_message(payload)
            if msg is not None and msg.WhichOneof("payload") is not None:
                messages.append(msg)
                self._record_server_message(msg)
            else:
                # 非 protobuf 载荷或空消息（如服务端 opaque ACK 文本）
                self._check_opaque_payload(payload)
        return messages

    def _record_server_message(self, msg: pb.ServerMessage) -> None:
        which = msg.WhichOneof("payload")
        self.stats.rudp_received += 1
        if which == "assign_player":
            self.player_id = msg.assign_player.player_id
        elif which == "snapshot":
            self.stats.snapshots += 1
        elif which == "delta":
            self.stats.deltas += 1

    def _check_opaque_payload(self, payload: bytes) -> None:
        """检查非 protobuf 载荷：检测服务端 opaque 文本 ACK 等。"""
        try:
            text = payload.decode("utf-8", errors="replace")
            # Phase 4.1：服务端发送固定前缀 "ORK-UDP-ACK:" 而非全文匹配
            if text.startswith("ORK-UDP-ACK:"):
                self._udp_ack_received = True
                self.stats.rudp_received += 1
        except Exception:
            pass

    def _wait_for_udp_ack(self, timeout: float) -> bool:
        """等待 UDP ACK。Phase 3.5: 服务端发送 opaque 文本 ACK 而非 protobuf。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump_rudp()
            # Phase 3.5: opaque 文本 ACK（唯一路径，不依赖 protobuf ACK）
            if self._udp_ack_received:
                return True
            time.sleep(0.005)
        return False

    def _wait_for_player_id(self, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.player_id:
                return True
            self.pump_rudp()
            time.sleep(0.005)
        return False

    def send_rudp(self, payload: bytes) -> None:
        """通过 RUDP 通道发送（概念称呼，底层使用 KCP 库）。"""
        assert self._rudp is not None
        if self.cipher is not None:
            payload = seal(self.cipher, CHANNEL_RUDP, C2S, pb.CLIENT_COMMAND, payload)
            if payload is None:
                raise RuntimeError("RUDP seal failed")
        self._rudp.send(payload)
        self.stats.rudp_bytes_tx += len(payload)

    def send_client_message(self, msg: pb.ClientMessage, *, channel: str = "rudp") -> int:
        """发送完整 ClientMessage（已按 proto 填充）。"""
        if channel == "kcp":
            channel = "rudp"
        payload = msg.SerializeToString()
        if channel == "rudp":
            self.send_rudp(payload)
            which = msg.WhichOneof("payload")
            if which == "ack":
                self.stats.acks_sent += 1
                self.stats.ack_bytes += len(payload)
            elif which == "reliable_cmd":
                self.stats.reliable_cmds_sent += 1
                self.stats.reliable_bytes += len(payload)
            return len(payload)

        if channel == "udp":
            assert self._udp_sock is not None
            if self.cipher is not None:
                payload = seal(self.cipher, CHANNEL_UDP, C2S, pb.CLIENT_COMMAND, payload)
                if payload is None:
                    raise RuntimeError("UDP seal failed")
            packet = wrap_udp_data(self.connection_id, payload)
            self._wire_sendto(packet, self._server_udp())
            which = msg.WhichOneof("payload")
            if which == "sync_pose":
                self.stats.poses_sent += 1
                self.stats.pose_bytes += len(packet)
            elif which == "entity_batch":
                self.stats.entity_batches_sent += 1
                self.stats.entity_batch_bytes += len(packet)
            return len(packet)

        raise ValueError(f"unknown channel: {channel}")

    def send_rich_pose(
        self,
        position: tuple[float, float, float],
        rotation: tuple[float, float, float, float],
        *,
        velocity: tuple[float, float, float] | None = None,
        angular_velocity: tuple[float, float, float] | None = None,
        animation_state_id: int = 0,
        bone_count: int = 0,
        device_metadata: dict[str, str] | None = None,
        timestamp_ms: int | None = None,
    ) -> int:
        assert self._udp_sock is not None
        ts = timestamp_ms if timestamp_ms is not None else int(time.time() * 1000)
        payload = build_rich_sync_pose_message(
            self.player_id,
            position,
            rotation,
            ts,
            velocity=velocity,
            angular_velocity=angular_velocity,
            animation_state_id=animation_state_id,
            bone_count=bone_count,
            device_metadata=device_metadata,
        )
        if self.cipher is not None:
            payload = seal(self.cipher, CHANNEL_UDP, C2S, pb.CLIENT_COMMAND, payload)
            if payload is None:
                raise RuntimeError("UDP seal failed (rich_pose)")
        packet = wrap_udp_data(self.connection_id, payload)
        self._wire_sendto(packet, self._server_udp())
        self.stats.poses_sent += 1
        self.stats.pose_bytes += len(packet)
        return len(packet)

    def send_reliable_gameplay(
        self,
        command_seq: int,
        mode: str,
        *,
        interaction_target: int = 1001,
    ) -> int:
        kwargs: dict = {"player_id": self.player_id, "command_seq": command_seq}
        if mode == "interaction":
            kwargs["interaction_target"] = interaction_target
            kwargs["interaction_type"] = "activate"
        elif mode == "equipment":
            kwargs["equipment_slots"] = [
                (0, 101 + (command_seq % 5), 100),
                (1, 201 + (command_seq % 3), 80),
                (2, 301, 50),
            ]
        elif mode == "chat":
            kwargs["chat_message"] = f"[unity-sim] hello from player {self.player_id} seq={command_seq}"
        else:
            raise ValueError(f"unknown reliable mode: {mode}")

        payload = build_reliable_gameplay_message(**kwargs)
        self.send_rudp(payload)
        self.stats.reliable_cmds_sent += 1
        self.stats.reliable_bytes += len(payload)
        return len(payload)

    def send_entity_batch(self, entity_count: int, *, base_entity_id: int = 5000) -> int:
        assert self._udp_sock is not None
        now_ms = int(time.time() * 1000)
        entities: list[tuple[int, tuple[float, float, float], tuple[float, float, float, float], int]] = []
        for i in range(entity_count):
            angle = i * 0.4
            pos = (math.cos(angle) * 3.0, 0.5 + i * 0.1, math.sin(angle) * 3.0)
            half = angle * 0.5
            rot = (0.0, math.sin(half), 0.0, math.cos(half))
            entities.append((base_entity_id + i, pos, rot, now_ms))

        payload = build_entity_batch_message(self.player_id, entities, now_ms)
        if self.cipher is not None:
            payload = seal(self.cipher, CHANNEL_UDP, C2S, pb.CLIENT_COMMAND, payload)
            if payload is None:
                raise RuntimeError("UDP seal failed (entity_batch)")
        packet = wrap_udp_data(self.connection_id, payload)
        self._wire_sendto(packet, self._server_udp())
        self.stats.entity_batches_sent += 1
        self.stats.entity_batch_bytes += len(packet)
        return len(packet)

    def send_ack(self, last_ack_step: int) -> int:
        payload = build_client_ack(self.connection_id, last_ack_step)
        self.send_rudp(payload)
        self.stats.acks_sent += 1
        self.stats.ack_bytes += len(payload)
        return len(payload)

    # ------------------------------------------------------------------ #
    # Phase 4.0: ORKBENCH1 benchmark ping（双通道）
    # ------------------------------------------------------------------ #

    def send_bench_reliable(self, sequence: int, send_time_ns: int) -> int:
        """通过 RUDP（Reliable）通道发送 ORKBENCH1 ping；返回发送字节数。"""
        payload = build_bench_payload(sequence, send_time_ns)
        self.send_rudp(payload)
        return len(payload)

    def send_bench_unreliable(self, sequence: int, send_time_ns: int) -> int:
        """通过 UDP（Unreliable）通道发送 ORKBENCH1 ping；返回发送字节数。"""
        assert self._udp_sock is not None
        payload = build_bench_payload(sequence, send_time_ns)
        if self.cipher is not None:
            payload = seal(self.cipher, CHANNEL_UDP, C2S, pb.CLIENT_COMMAND, payload)
            if payload is None:
                raise RuntimeError("UDP seal failed (bench)")
        packet = wrap_udp_data(self.connection_id, payload)
        self._wire_sendto(packet, self._server_udp())
        return len(packet)

    def drain_bench_rudp(self) -> list[tuple[int, int]]:
        """消费并返回 RUDP 收到的 ORKBENCH1 回显列表 [(seq, send_time_ns), ...]。"""
        result = []
        for raw in self._bench_rudp_rx:
            parsed = parse_bench_payload(raw)
            if parsed is not None:
                result.append(parsed)
        self._bench_rudp_rx.clear()
        return result

    def drain_bench_udp(self) -> list[tuple[int, int]]:
        """消费并返回 UDP 收到的 ORKBENCH1 回显列表 [(seq, send_time_ns), ...]。"""
        result = []
        for raw in self._bench_udp_rx:
            parsed = parse_bench_payload(raw)
            if parsed is not None:
                result.append(parsed)
        self._bench_udp_rx.clear()
        return result

    def send_leave(self) -> None:
        self.send_rudp(build_leave_message(self.player_uuid))
        self.pump_rudp()

    def close(self) -> None:
        self._stop.set()
        # Phase 6.2: 先 flush 损伤引擎的延迟发送队列（此时 UDP socket 仍开），
        # 再关 socket，确保已入队的尾包不丢、后台调度线程干净退出。
        if self._impairment is not None:
            self._impairment.stop()
        if self._udp_sock is not None:
            try:
                self._udp_sock.close()
            except OSError:
                pass
            self._udp_sock = None
        if self._udp_thread is not None:
            self._udp_thread.join(timeout=0.5)
            self._udp_thread = None
        if self._rudp is not None:
            self._rudp.close()
            self._rudp = None
        if self._tcp_sock is not None:
            try:
                self._tcp_sock.close()
            except OSError:
                pass
            self._tcp_sock = None


class UnityPoseSimulator:
    """模拟 Unity Transform：圆周移动 + Yaw 旋转 + 速度向量。"""

    def __init__(self, seed: float, *, radius: float = 8.0, height: float = 1.6, speed: float = 2.0):
        self.radius = radius
        self.height = height
        self.speed = speed
        self.phase = seed
        self._last_pos: tuple[float, float, float] | None = None
        self._last_t = 0.0

    def sample(
        self, t: float
    ) -> tuple[
        tuple[float, float, float],
        tuple[float, float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
        int,
    ]:
        angle = self.phase + t * self.speed
        x = math.cos(angle) * self.radius
        z = math.sin(angle) * self.radius
        y = self.height + math.sin(t * 3.0 + self.phase) * 0.15

        half = (angle + math.pi / 2) * 0.5
        rotation = (0.0, math.sin(half), 0.0, math.cos(half))
        position = (x, y, z)

        if self._last_pos is not None and t > self._last_t:
            dt = t - self._last_t
            velocity = (
                (position[0] - self._last_pos[0]) / dt,
                (position[1] - self._last_pos[1]) / dt,
                (position[2] - self._last_pos[2]) / dt,
            )
            angular_velocity = (0.0, self.speed, 0.0)
        else:
            velocity = (0.0, 0.0, 0.0)
            angular_velocity = (0.0, 0.0, 0.0)

        self._last_pos = position
        self._last_t = t
        anim_id = int(1000 + (math.sin(t) + 1) * 500)

        return position, rotation, velocity, angular_velocity, anim_id
