"""Ork 客户端会话封装 — demo 精简版（全 opaque，零 protobuf 依赖）。

从 harmonia_ork `demo/python/client/ork_client_session.py`（~1214 行，含完整
protobuf 业务消息构建）精简而来：剥除 Proto / Join / 房间业务 / 弱网损伤等
与"纯传输 SDK 演示"无关的部分，只保留建立连接与双通道 opaque 收发所需的最小
闭环，同时 **完整保留线协议兼容性**：

  TCP 连接 → Bootstrap V2（109B opaque 帧，4B 大端 length prefix）
  → 生成 clientNonce → UDP bind + KCP 会话（conv = connectionId）
  → UDP Handshake V2（42B 认证帧）→ 收到服务端 "ORK-UDP-ACK:"（KCP 可靠通道）
  → Connected，可在 Reliable(KCP) / Unreliable(UDP) 两通道收发 opaque 载荷。

加密：CipherState 依据 Bootstrap 帧的 encryption_required 标志自动在
ChaCha20-Poly1305-IETF(AEAD) 与明文两模式间切换（与 C++ OrkCipher::Seal/Open
逐字节对齐）；明文模式同样保留 15B OrkFrame 帧头。

回显约束：标准 demo 服务端（TransportDemoHandler）仅对 **精确 25B、前缀
"ORKBENCH1"** 的 opaque 帧按原通道回显；其余字节按 proto 解析、失败即丢弃。
因此本模块提供 build_bench_payload / send_bench_* 走该回显路径。

packet_type：seal 写入 15B 帧头的 packet_type 取 proto 枚举 CLIENT_COMMAND(=5)，
与参考客户端一致；服务端 Open 时以帧内嵌入值重建 AAD，故收发自洽。
"""

from __future__ import annotations

import os
import socket
import struct
import threading
import time
from dataclasses import dataclass, field

from .kcp.ikcp_binding import RudpSession
from .ork_cipher import (
    C2S,
    S2C,
    CHANNEL_RUDP,
    CHANNEL_UDP,
    CipherState,
    open_frame,
    seal,
)
from .ork_frame import (
    BOOTSTRAP_FRAME_SIZE,
    UDP_HANDSHAKE_V2_SIZE,
    UDP_HANDSHAKE_V2_TYPE,
    build_handshake_v2_packet,
    decode_bootstrap_frame,
    handshake_counter_can_advance,
)

# ── UDP 线协议常量（与服务端 UdpTransportServer 一致）───────────────────────
UDP_HANDSHAKE_V2 = 3   # UdpPacketType::HandshakeV2
UDP_DATA = 2           # UdpPacketType::Data
# UDP 数据包头：type(1B) + connectionId(4B BE) + payloadSize(2B BE) = 7B
UDP_DATA_HEADER_SIZE = 7

# seal 写入帧头的 packet_type（proto 枚举 MessageType.CLIENT_COMMAND）
CLIENT_COMMAND = 5

# 服务端握手 ACK 前缀（NetworkTransport::SendHandshakeAck → "ORK-UDP-ACK:handshake"）
UDP_ACK_PREFIX = "ORK-UDP-ACK:"

# ── ORKBENCH1 opaque benchmark ping（服务端唯一回显路径）───────────────────
# 帧格式：prefix(9B "ORKBENCH1") + seq(8B LE) + send_time_ns(8B LE) = 25B
ORKBENCH_PREFIX = b"ORKBENCH1"
ORKBENCH_SIZE = 25  # 9 + 8 + 8


# --------------------------------------------------------------------------- #
# TCP length-prefixed framing helpers
# --------------------------------------------------------------------------- #


def read_exact(sock: socket.socket, n: int) -> bytes:
    """从 TCP socket 精确读取 n 字节；对端关闭抛 ConnectionError。"""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("TCP 连接已关闭")
        buf += chunk
    return buf


def read_tcp_length_prefixed(sock: socket.socket, *, expected_length: int | None = None) -> bytes:
    """读取 4B 大端 length prefix + payload（Bootstrap V2 硬切）。

    - `length` 先做合理上限校验（≤ 4 MiB），防畸形长度导致超量读取；
    - `expected_length` 给定时，声明的 length 必须严格相等（外层 prefix 冻结
      语义：Bootstrap V2 必须等于 109，且不含 prefix 自身 4 字节）。
    """
    (length,) = struct.unpack("!I", read_exact(sock, 4))
    if length == 0 or length > 4 * 1024 * 1024:
        raise ValueError(f"非法 TCP 消息长度: {length}")
    if expected_length is not None and length != expected_length:
        raise ValueError(f"非法 TCP 消息长度: {length}（期望 {expected_length}）")
    return read_exact(sock, length)


# --------------------------------------------------------------------------- #
# ORKBENCH1 payload helpers（25B opaque 回显帧）
# --------------------------------------------------------------------------- #


def build_bench_payload(sequence: int, send_time_ns: int) -> bytes:
    """构建 ORKBENCH1 benchmark ping payload（精确 25 字节）。"""
    return ORKBENCH_PREFIX + struct.pack("<QQ", sequence, send_time_ns)


def parse_bench_payload(data: bytes) -> tuple[int, int] | None:
    """解析 ORKBENCH1 payload → (sequence, send_time_ns)；不匹配返回 None。"""
    if len(data) != ORKBENCH_SIZE or data[:9] != ORKBENCH_PREFIX:
        return None
    seq, ts = struct.unpack("<QQ", data[9:25])
    return seq, ts


def wrap_udp_data(connection_id: int, payload: bytes) -> bytes:
    """为 UDP 不可靠通道包裹 7B UdpDataHeader（type=Data, connId, size）。"""
    header = struct.pack("!BIH", UDP_DATA, connection_id, len(payload))
    return header + payload


def wrap_udp_handshake(
    connection_id: int,
    client_nonce: bytes,
    counter: int,
    bootstrap_epoch: bytes,
    c2s_key: bytes,
) -> bytes:
    """构造 42B Handshake V2 握手包（Rev 3 §5.5/§5.6）。

    handshakeKey 由 c2sKey 经 HKDF-SHA-256 派生（info label "ORK-UDP-HS-C2S-v2"），
    不复用数据通道密钥；authTag 覆盖除自身外全部 26B 明文字段。
    """
    return build_handshake_v2_packet(
        connection_id, client_nonce, counter, bootstrap_epoch, c2s_key
    )


# --------------------------------------------------------------------------- #
# 会话统计
# --------------------------------------------------------------------------- #


@dataclass
class SessionStats:
    tcp_bytes_rx: int = 0
    tcp_bytes_tx: int = 0
    rudp_bytes_tx: int = 0            # Reliable(KCP) 通道发送字节
    udp_bytes_tx: int = 0             # Unreliable(UDP) 通道发送字节（含 7B 头）
    rudp_received: int = 0            # KCP 通道收到的 opaque 载荷计数
    udp_received: int = 0             # UDP 通道收到的 opaque 载荷计数
    bench_rudp_echoes: int = 0        # KCP 通道 ORKBENCH1 回显计数
    bench_udp_echoes: int = 0         # UDP 通道 ORKBENCH1 回显计数
    errors: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------- #
# 单客户端完整会话
# --------------------------------------------------------------------------- #


class OrkClientSession:
    """单客户端完整会话：TCP bootstrap → UDP/KCP → 双通道 opaque 收发。

    典型用法::

        with OrkClientSession("127.0.0.1", 9400) as s:
            s.connect()                       # bootstrap + handshake + wait ACK
            s.send_bench_reliable(seq, ts)    # Reliable(KCP)
            s.pump_rudp()                     # 驱动 KCP 收发
            echoes = s.drain_bench_rudp()

    全部收发载荷均为 opaque bytes，不依赖 protobuf。
    """

    def __init__(
        self,
        host: str,
        port: int,
        player_uuid: str = "ork-demo-client",
        *,
        connect_timeout: float = 5.0,
        ack_timeout: float = 10.0,
    ):
        self.host = host
        self.port = port
        self.player_uuid = player_uuid
        self.connect_timeout = connect_timeout
        self.ack_timeout = ack_timeout

        self.connection_id = 0
        self.session_token = 0
        self.bootstrap_epoch = b""       # 16B 安全域标识（不写日志）
        self.encryption_required = False

        # Handshake V2 客户端状态（Rev 3 §5.8）：nonce 为 16B CSPRNG，counter
        # 从 0 起、重试同 nonce 严格递增；密钥仅驻留内存，不日志、不序列化。
        self._hs_client_nonce: bytes | None = None
        self._hs_counter: int = 0
        self._hs_c2s_key: bytes = b""

        self.cipher: CipherState | None = None

        self._tcp_sock: socket.socket | None = None
        self._udp_sock: socket.socket | None = None
        self._rudp: RudpSession | None = None
        self._stop = threading.Event()
        self._udp_thread: threading.Thread | None = None
        self._clock_base = 0.0
        self.stats = SessionStats()

        self._rx_payloads: list[bytes] = []       # KCP 通道非 bench opaque 载荷
        self._udp_rx_payloads: list[bytes] = []   # UDP 通道非 bench opaque 载荷
        self._bench_rudp_rx: list[bytes] = []     # ORKBENCH1 KCP 回显
        self._bench_udp_rx: list[bytes] = []      # ORKBENCH1 UDP 回显
        self._udp_ack_received = False

    # -- 上下文管理 ----------------------------------------------------------
    def __enter__(self) -> "OrkClientSession":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- 内部工具 ------------------------------------------------------------
    def _server_udp(self) -> tuple[str, int]:
        return (self.host, self.port)

    def rudp_now_ms(self) -> int:
        """返回 KCP 时钟（相对 start_udp_rudp 的毫秒数）。"""
        return int((time.monotonic() - self._clock_base) * 1000)

    # -- 步骤 1：TCP bootstrap ------------------------------------------------
    def connect_tcp_bootstrap(self) -> str:
        """TCP 连接并接收 Bootstrap V2 帧（109B opaque，硬切单轨）。"""
        tcp_addr = (self.host, self.port)
        self._tcp_sock = socket.create_connection(tcp_addr, timeout=self.connect_timeout)
        self._tcp_sock.settimeout(self.connect_timeout)

        # 外层 4B 大端 length-prefix 必须严格 = 109（不含 prefix 自身）
        raw = read_tcp_length_prefixed(self._tcp_sock, expected_length=BOOTSTRAP_FRAME_SIZE)
        self.stats.tcp_bytes_rx += 4 + len(raw)

        if len(raw) != BOOTSTRAP_FRAME_SIZE:
            raise RuntimeError(f"BootstrapFrame 长度非法: {len(raw)} != {BOOTSTRAP_FRAME_SIZE}")

        bootstrap = decode_bootstrap_frame(raw)
        self.connection_id = bootstrap["connection_id"]
        self.session_token = bootstrap["session_token"]
        self.bootstrap_epoch = bootstrap["bootstrap_epoch"]
        self.encryption_required = bootstrap["encryption_required"]

        # 每次成功解析 Bootstrap V2 后生成新的 16B ClientNonce（CSPRNG），
        # counter 从 0 开始；后续重试同 nonce 严格递增。
        self._hs_client_nonce = os.urandom(16)
        self._hs_counter = 0
        self._hs_c2s_key = bootstrap["c2s_key"]

        # 初始化 cipher（明文模式也需要，用于帧头解析 + replay 跟踪）
        self.cipher = CipherState.from_bootstrap(bootstrap)

        enc_label = "encrypted" if self.encryption_required else "plaintext"
        # summary 不含 session_token 值（凭证材料）；connectionId 可记录。
        return f"BootstrapFrame conn={self.connection_id} {enc_label}"

    # -- 步骤 2：UDP + KCP ----------------------------------------------------
    def start_udp_rudp(self) -> None:
        """绑定 UDP socket 并创建 KCP 会话（conv = connectionId），启动收包线程。"""
        self._udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp_sock.bind(("0.0.0.0", 0))
        self._udp_sock.settimeout(0.02)

        def rudp_output(data: bytes) -> None:
            assert self._udp_sock is not None
            self._udp_sock.sendto(data, self._server_udp())

        self._rudp = RudpSession(self.connection_id, rudp_output)
        self._clock_base = time.monotonic()

        self._udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        self._udp_thread.start()

    # 兼容旧脚本名
    start_udp_kcp = start_udp_rudp

    def send_udp_handshake(self) -> None:
        """发送 UDP Handshake V2（42B 认证帧）。

        同一 bootstrap 会话内 nonce 固定、counter 单调递增；counter 溢出
        fail closed（须重建连接，不回绕）。
        """
        assert self._udp_sock is not None
        if self._hs_client_nonce is None or not self._hs_c2s_key:
            raise RuntimeError("Handshake V2 状态缺失（bootstrap 未完成）")
        if not handshake_counter_can_advance(self._hs_counter):
            raise RuntimeError("handshake counter exhausted (fail closed; reconnect required)")
        packet = wrap_udp_handshake(
            self.connection_id,
            self._hs_client_nonce,
            self._hs_counter,
            self.bootstrap_epoch,
            self._hs_c2s_key,
        )
        self._udp_sock.sendto(packet, self._server_udp())

    def wait_udp_ack(self, timeout: float | None = None, *, retry_interval: float = 0.5) -> str:
        """等待 KCP opaque "ORK-UDP-ACK:"（握手重试：同 nonce、counter 严格递增）。

        每 retry_interval 秒未收到 ACK 则重发握手帧（counter +1）；服务端幂等：
        首次绑定发 ACK，重试帧仅重发 ACK 不重复 Connected。
        """
        if timeout is None:
            timeout = self.ack_timeout
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("未收到 UdpAck")
            if self._wait_for_udp_ack(timeout=min(retry_interval, remaining)):
                return "ORK-UDP-ACK:handshake"
            if not handshake_counter_can_advance(self._hs_counter):
                raise RuntimeError("handshake counter exhausted (fail closed; reconnect required)")
            self._hs_counter += 1
            self.send_udp_handshake()

    def connect(self, *, timeout: float | None = None) -> str:
        """一站式建立连接：bootstrap → UDP/KCP → handshake → wait ACK。

        返回连接摘要字符串（含 conn id 与加密模式）。
        """
        summary = self.connect_tcp_bootstrap()
        self.start_udp_rudp()
        self.send_udp_handshake()
        ack = self.wait_udp_ack(timeout=timeout)
        return f"{summary} | {ack}"

    # -- UDP 收包后台线程 ----------------------------------------------------
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
            if self._is_udp_data_packet(packet):
                # 服务端 UdpDataHeader 包裹的 OrkFrame：解帧并计收包
                self._handle_udp_data_packet(packet)
                continue
            # 其他：原始 KCP 包
            try:
                self._rudp.input(packet)
            except RuntimeError as exc:
                self.stats.errors.append(f"KCP input: {exc}")

    def _is_udp_data_packet(self, packet: bytes) -> bool:
        """检测服务端 UdpDataHeader 包裹的 UDP 包（type=0x02）。

        KCP 包首部 4B 是 conv=connection_id (LE)，当 connection_id 低位为 0x02
        会与 UDP_DATA type 碰撞。用长度交叉验证排除误判。
        """
        if len(packet) < UDP_DATA_HEADER_SIZE:
            return False
        if packet[0] != UDP_DATA:
            return False
        conn_id, payload_size = struct.unpack("!IH", packet[1:UDP_DATA_HEADER_SIZE])
        if conn_id == 0:
            return False  # connection_id 不应为 0
        return len(packet) == UDP_DATA_HEADER_SIZE + payload_size

    def _handle_udp_data_packet(self, packet: bytes) -> None:
        """解包 UdpDataHeader → OrkFrame → 解密 → 记录统计。"""
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
        if parse_bench_payload(payload) is not None:
            self.stats.bench_udp_echoes += 1
            self._bench_udp_rx.append(payload)
            return
        self._udp_rx_payloads.append(payload)

    # -- KCP 通道收发 --------------------------------------------------------
    def pump_rudp(self) -> list[bytes]:
        """驱动 KCP 收发，返回本轮收到的非 bench opaque 载荷列表。

        副作用：更新 UDP ACK 标记、把 ORKBENCH1 回显入队到 _bench_rudp_rx。
        """
        assert self._rudp is not None
        self._rudp.update(self.rudp_now_ms())
        out: list[bytes] = []
        while True:
            payload = self._rudp.recv()
            if payload is None:
                break
            if self.cipher is not None:
                payload = open_frame(self.cipher, CHANNEL_RUDP, S2C, 0, payload)
                if payload is None:
                    self.stats.errors.append("KCP decrypt/open failed")
                    continue
            # ORKBENCH1 回显优先拦截
            if parse_bench_payload(payload) is not None:
                self.stats.bench_rudp_echoes += 1
                self._bench_rudp_rx.append(payload)
                continue
            # 服务端 opaque 文本 ACK
            if self._looks_like_ack(payload):
                self._udp_ack_received = True
                self.stats.rudp_received += 1
                continue
            self.stats.rudp_received += 1
            out.append(payload)
            self._rx_payloads.append(payload)
        return out

    @staticmethod
    def _looks_like_ack(payload: bytes) -> bool:
        try:
            return payload.decode("utf-8", errors="strict").startswith(UDP_ACK_PREFIX)
        except Exception:
            return False

    def _wait_for_udp_ack(self, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump_rudp()
            if self._udp_ack_received:
                return True
            time.sleep(0.005)
        return False

    def send_rudp(self, payload: bytes) -> int:
        """通过 Reliable(KCP) 通道发送 opaque 载荷；返回上线字节数。"""
        assert self._rudp is not None
        if self.cipher is not None:
            sealed = seal(self.cipher, CHANNEL_RUDP, C2S, CLIENT_COMMAND, payload)
            if sealed is None:
                raise RuntimeError("KCP seal failed")
        else:
            sealed = payload
        self._rudp.send(sealed)
        self.stats.rudp_bytes_tx += len(sealed)
        return len(sealed)

    def send_udp(self, payload: bytes) -> int:
        """通过 Unreliable(UDP) 通道发送 opaque 载荷；返回上线字节数（含 7B 头）。"""
        assert self._udp_sock is not None
        if self.cipher is not None:
            sealed = seal(self.cipher, CHANNEL_UDP, C2S, CLIENT_COMMAND, payload)
            if sealed is None:
                raise RuntimeError("UDP seal failed")
        else:
            sealed = payload
        packet = wrap_udp_data(self.connection_id, sealed)
        self._udp_sock.sendto(packet, self._server_udp())
        self.stats.udp_bytes_tx += len(packet)
        return len(packet)

    # -- ORKBENCH1 双通道 ping ----------------------------------------------
    def send_bench_reliable(self, sequence: int, send_time_ns: int) -> int:
        """通过 Reliable(KCP) 通道发送 ORKBENCH1 ping；返回发送字节数。"""
        payload = build_bench_payload(sequence, send_time_ns)
        self.send_rudp(payload)
        return len(payload)

    def send_bench_unreliable(self, sequence: int, send_time_ns: int) -> int:
        """通过 Unreliable(UDP) 通道发送 ORKBENCH1 ping；返回上线字节数。"""
        payload = build_bench_payload(sequence, send_time_ns)
        return self.send_udp(payload)

    def drain_bench_rudp(self) -> list[tuple[int, int]]:
        """消费并返回 KCP 收到的 ORKBENCH1 回显 [(seq, send_time_ns), ...]。"""
        result = []
        for raw in self._bench_rudp_rx:
            parsed = parse_bench_payload(raw)
            if parsed is not None:
                result.append(parsed)
        self._bench_rudp_rx.clear()
        return result

    def drain_bench_udp(self) -> list[tuple[int, int]]:
        """消费并返回 UDP 收到的 ORKBENCH1 回显 [(seq, send_time_ns), ...]。"""
        result = []
        for raw in self._bench_udp_rx:
            parsed = parse_bench_payload(raw)
            if parsed is not None:
                result.append(parsed)
        self._bench_udp_rx.clear()
        return result

    def drain_udp_payloads(self) -> list[bytes]:
        """消费并返回 UDP 通道收到的非 bench opaque 载荷。"""
        out = list(self._udp_rx_payloads)
        self._udp_rx_payloads.clear()
        return out

    # -- 关闭 ----------------------------------------------------------------
    def close(self) -> None:
        self._stop.set()
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
