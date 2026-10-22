"""Phase 6.2 — 应用层网络损伤注入。

背景
----
`benchmark/netem.sh` 依赖 Linux tc/qdisc，在 macOS 上直接退出，历史
6 份基线报告全部落在 `network=unshaped`。为了让 macOS/CI 也能测出弱网
下的表现，本模块在**应用层收发路径**上注入损伤，作为内核整形（Linux
netem / macOS dnctl）之外的补充手段。

设计要点
--------
1. **同步实现**：`benchmark.py` 用线程池驱动多客户端，事件循环不在场，
   因此这里用阻塞式 API（`time.sleep`），而不是 asyncio。签名保留 spec
   中的 `impair_send(data, send_fn)` / `impair_recv(data)` 形状，只去掉
   `async` 关键字，语义完全一致。
2. **RTT / loss 语义对齐 netem**：netem.sh 用 `tc qdisc ... root netem`，
   root qdisc **只整形 egress**（出向）。故本引擎的 `delay_ms` / `loss_pct`
   / `dup_pct` / `bandwidth` **只作用于发送路径**；接收路径只做 reorder
   （便于客户端直接观测乱序到达），**不叠加 loss/delay**。这样往返存活
   率 = (1-loss_pct) 而非 (1-loss_pct)²，`rtt_p50` 上升幅度 ≈ `delay_ms`
   （Phase 6.2 门禁：mobile 档 60ms±20%、loss=5% 档 UDP 送达 ≈95%）。
   delay 通过**非阻塞后台队列**（`_DelayedSender`）复刻 netem 排队释放
   语义：burst 内多个 KCP 分段几乎同时入队、同时（+delay）释放，**不叠加**、
   也**不阻塞** benchmark 主循环（阻塞会虚高 RTT）。
3. **无损默认**：`ImpairmentProfile("unshaped")` 全 0，Engine 会短路返回，
   等价于透传。上层通过 `profile is None` 或 `engine is None` 完全跳过。
4. **可测量性**：Engine 内部累计 `sent / dropped_send / duplicated /
   received / dropped_recv / reordered / total_delay_seconds`，供 benchmark
   报告落盘（后续可挂进 `summarize()`）。
5. **线程安全**：一个 Engine 实例内部用 `threading.Lock` 保护 RNG 与
   reorder buffer；推荐**每个客户端线程持有独立 Engine**，避免锁竞争。

三档预定义与 Linux `netem.sh` 保持参数对齐：
- broadband: delay 20ms  jitter 2ms   loss 0.1%
- mobile   : delay 60ms  jitter 10ms  loss 1.0%  reorder 0.2%
- weak     : delay 120ms jitter 30ms  loss 3.0%  reorder 1.0%
"""

from __future__ import annotations

import heapq
import itertools
import random
import threading
import time
from dataclasses import asdict, dataclass
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Profile
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ImpairmentProfile:
    """一档网络损伤参数。

    字段语义与 Linux netem 对齐；`bandwidth_kbps=0` 表示不限速。
    `frozen=True` 让 profile 可以在多线程间安全共享。
    """

    name: str
    delay_ms: float = 0.0        # 基础单向延迟（仅作用于发送路径）
    jitter_ms: float = 0.0       # 抖动标准差（正态分布，均值 0）
    loss_pct: float = 0.0        # 丢包率 %（0-100）
    reorder_pct: float = 0.0     # 乱序率 %（0-100）
    dup_pct: float = 0.0         # 重复率 %（0-100）
    bandwidth_kbps: int = 0      # 令牌桶限速 kbps；0=不限

    def is_passthrough(self) -> bool:
        """全 0 表示不注入任何损伤。"""
        return (
            self.delay_ms <= 0.0
            and self.jitter_ms <= 0.0
            and self.loss_pct <= 0.0
            and self.reorder_pct <= 0.0
            and self.dup_pct <= 0.0
            and self.bandwidth_kbps <= 0
        )

    def to_dict(self) -> dict:
        return asdict(self)


# 三档预定义 + unshaped。名字必须与 benchmark.py --network / netem.sh 一致。
PROFILES: dict[str, ImpairmentProfile] = {
    "unshaped": ImpairmentProfile("unshaped"),
    "broadband": ImpairmentProfile(
        "broadband",
        delay_ms=20.0,
        jitter_ms=2.0,
        loss_pct=0.1,
    ),
    "mobile": ImpairmentProfile(
        "mobile",
        delay_ms=60.0,
        jitter_ms=10.0,
        loss_pct=1.0,
        reorder_pct=0.2,
    ),
    "weak": ImpairmentProfile(
        "weak",
        delay_ms=120.0,
        jitter_ms=30.0,
        loss_pct=3.0,
        reorder_pct=1.0,
    ),
}


def get_profile(name: Optional[str]) -> ImpairmentProfile:
    """按名称取预定义 profile；`None` / 空串 / `unshaped` 一律返回透传档。

    未知名称抛 `ValueError`，让调用方（benchmark.py）走 argparse 报错路径，
    而不是静默降级成 unshaped。
    """
    if name is None:
        return PROFILES["unshaped"]
    key = name.strip()
    if not key or key == "unshaped":
        return PROFILES["unshaped"]
    if key not in PROFILES:
        raise ValueError(
            f"未知 network profile: {name!r}；可选: {sorted(PROFILES.keys())}"
        )
    return PROFILES[key]


# ---------------------------------------------------------------------------
# Token bucket（bandwidth_kbps）
# ---------------------------------------------------------------------------


class TokenBucket:
    """极简令牌桶：按 `bandwidth_kbps * 1000 / 8` 字节/秒补充。

    - 容量 = 125ms 的字节数（允许短突发，避免小包被过度节流），下限 1500B。
    - `acquire(size)` 阻塞直到桶里有足够令牌；单次 acquire 允许超过容量
      （大 payload 会分多次等待，避免死锁）。
    """

    def __init__(self, bandwidth_kbps: int):
        if bandwidth_kbps <= 0:
            raise ValueError("bandwidth_kbps must be > 0")
        self.rate_bytes_per_sec = bandwidth_kbps * 1000.0 / 8.0
        self.capacity = max(1500.0, self.rate_bytes_per_sec * 0.125)
        self.tokens = self.capacity
        self.last_refill = time.monotonic()
        self._lock = threading.Lock()

    def _refill_locked(self) -> None:
        now = time.monotonic()
        elapsed = now - self.last_refill
        if elapsed > 0:
            self.tokens = min(self.capacity, self.tokens + elapsed * self.rate_bytes_per_sec)
            self.last_refill = now

    def acquire(self, size: int) -> float:
        """阻塞直到消耗掉 size 字节令牌；返回累计等待秒数。"""
        if size <= 0:
            return 0.0
        waited = 0.0
        with self._lock:
            while True:
                self._refill_locked()
                if self.tokens >= size:
                    self.tokens -= size
                    return waited
                deficit = size - self.tokens
                wait = deficit / self.rate_bytes_per_sec
                # 释放锁再睡，允许其它线程 refill
                self._lock.release()
                try:
                    time.sleep(wait)
                    waited += wait
                finally:
                    self._lock.acquire()


# ---------------------------------------------------------------------------
# Delayed sender（非阻塞延迟队列）
# ---------------------------------------------------------------------------


class _DelayedSender:
    """后台发送线程：按 `release_time` 释放待发包，复刻 netem 排队延迟。

    为什么需要它（Phase 6.2 关键修正）
    ----------------------------------
    早期实现用**阻塞 `time.sleep(delay)`** 在发送前等待。这在"单包请求-
    应答"路径（reference udp_only）上没问题，但在 ork_hybrid 上会**双重
    失真**：

    1. **延迟叠加**：KCP 把一条逻辑消息拆成多个分段，`_drain_output` 在
       一个 burst 内连续回调 output；若每个分段都阻塞 sleep(delay)，N 个
       分段就累计 N×delay（实测 RUDP RTT 涨到 ~3×60ms）。
    2. **阻塞主循环**：`pump_rudp()` 触发的 KCP 输出也走同一发送出口，
       sleep 会卡住 benchmark 主循环，导致回显无法及时 drain，连 UDP 通道
       的 RTT 都被虚高（实测 ~2×60ms）。

    netem 的真实语义是：每个包在**入队时刻**被打上 `release = enqueue +
    delay` 时间戳，由 qdisc 并发持有、到点释放——burst 内的包几乎同时
    入队，也就几乎同时（+delay）释放，**不叠加**，且**不阻塞**发送方。

    本类用一个后台线程 + 最小堆精确复刻该语义：`schedule()` 入队即返回
    （非阻塞），后台线程到点调用 `send_fn`。
    """

    def __init__(self) -> None:
        self._heap: list[tuple[float, int, bytes, Callable[[bytes], None]]] = []
        self._counter = itertools.count()
        self._cond = threading.Condition()
        self._stopped = False
        self._thread = threading.Thread(
            target=self._run, name="impairment-delayed-sender", daemon=True
        )
        self._thread.start()

    def schedule(self, release_time: float, data: bytes,
                 send_fn: Callable[[bytes], None]) -> None:
        """把 (data, send_fn) 排入队列，到 `release_time`（monotonic）再发。"""
        with self._cond:
            heapq.heappush(
                self._heap, (release_time, next(self._counter), data, send_fn)
            )
            self._cond.notify()

    def _run(self) -> None:
        while True:
            with self._cond:
                while not self._heap and not self._stopped:
                    self._cond.wait()
                if not self._heap and self._stopped:
                    return
                now = time.monotonic()
                release_time, _seq, data, send_fn = self._heap[0]
                # 未停机且未到点 → 定时等待；停机时忽略 release_time 立即 flush
                if release_time > now and not self._stopped:
                    self._cond.wait(timeout=release_time - now)
                    continue
                heapq.heappop(self._heap)
            # 锁外调用 send_fn：避免发送阻塞影响其它包入队/释放
            try:
                send_fn(data)
            except Exception:  # noqa: BLE001 — socket 已关等，吞掉不让线程崩
                pass

    def stop(self, timeout: float = 2.0) -> None:
        """停机：flush 剩余待发包（立即发送）后 join 线程。幂等。"""
        with self._cond:
            already = self._stopped
            self._stopped = True
            self._cond.notify_all()
        if not already:
            self._thread.join(timeout)


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------


class ImpairmentEngine:
    """在发送/接收路径注入网络损伤。

    典型用法（同步 API，与 spec 一致，仅去掉 async）::

        engine = ImpairmentEngine(PROFILES["mobile"], seed=42)

        # 发送：delay + loss + dup + bandwidth
        engine.impair_send(payload, lambda b: sock.sendto(b, addr))

        # 接收：loss + reorder（不加 delay，避免 RTT 被双向叠加）
        out = engine.impair_recv(packet)
        if out is not None:
            handle(out)

    Engine 是**有状态**的：内部维护 reorder buffer 与统计计数器。推荐
    **每客户端线程一个实例**，避免共享锁竞争；不同 engine 之间不共享
    reorder buffer，也就不会出现"UDP 缓冲的包被 RUDP 通道释放"的语义混乱。
    """

    # reorder buffer 上限：防止极端配置下无限增长；超过后强制释放最老的一个
    _REORDER_BUF_MAX = 8

    def __init__(self, profile: ImpairmentProfile, *, seed: Optional[int] = None):
        self.profile = profile
        # 每个 engine 独立 RNG，避免多线程争抢 random 全局锁
        self._rng = random.Random(seed)
        self._rng_lock = threading.Lock()
        self._bucket: Optional[TokenBucket] = (
            TokenBucket(profile.bandwidth_kbps) if profile.bandwidth_kbps > 0 else None
        )
        self._reorder_buf: list[bytes] = []
        # 统计（供 benchmark 报告与调试）
        self.sent_count = 0
        self.dropped_send_count = 0
        self.dup_count = 0
        self.recv_count = 0
        self.dropped_recv_count = 0
        self.reorder_hold_count = 0
        self.reorder_release_count = 0
        self.total_delay_seconds = 0.0
        self.total_throttle_seconds = 0.0
        # Phase 6.2 修正：仅当 profile 含 delay/jitter 时才起后台发送线程，
        # 非阻塞地复刻 netem 排队延迟（见 _DelayedSender 文档）。纯 loss/dup
        # 档不起线程，发送保持内联、零额外开销、计数即时可见。
        self._sender: Optional[_DelayedSender] = (
            _DelayedSender()
            if (profile.delay_ms > 0.0 or profile.jitter_ms > 0.0)
            else None
        )

    # -- 内部工具 --------------------------------------------------------

    def _rand(self) -> float:
        with self._rng_lock:
            return self._rng.random()

    def _gauss(self, mu: float, sigma: float) -> float:
        with self._rng_lock:
            return self._rng.gauss(mu, sigma)

    def _sample_delay_seconds(self) -> float:
        p = self.profile
        base = p.delay_ms / 1000.0
        if p.jitter_ms > 0:
            # 正态抖动，均值 0，标准差 jitter_ms；截断到 >= 0
            return max(0.0, base + self._gauss(0.0, p.jitter_ms / 1000.0))
        return base

    # -- 发送路径 --------------------------------------------------------

    def impair_send(self, data: bytes, send_fn: Callable[[bytes], None]) -> bool:
        """发送路径注入：loss → bandwidth → delay → send → dup。

        返回值：True 表示实际调用了 `send_fn`（含 dup 时的两次），False 表示
        在 loss 阶段被丢弃、根本没上线。
        """
        p = self.profile
        if p.is_passthrough():
            send_fn(data)
            self.sent_count += 1
            return True

        # 1) 丢包（先判，丢掉的包不占带宽/不产生延迟）
        if p.loss_pct > 0 and self._rand() * 100.0 < p.loss_pct:
            self.dropped_send_count += 1
            return False

        # 2) 令牌桶限速（阻塞式节流；三档预定义 bandwidth_kbps=0，通常不触发）
        if self._bucket is not None:
            self.total_throttle_seconds += self._bucket.acquire(len(data))

        # 3) 延迟 + 4) 发送 + 5) 重复
        #    含 delay 时走**非阻塞调度**：入队即返回，由后台线程到点发送，
        #    复刻 netem 排队语义（burst 内多包不叠加、不阻塞主循环）。
        #    无 delay（纯 loss/dup 档）时内联发送，零额外开销、计数即时。
        delay = self._sample_delay_seconds()
        duplicate = p.dup_pct > 0 and self._rand() * 100.0 < p.dup_pct
        if delay > 0 and self._sender is not None:
            self.total_delay_seconds += delay
            release_at = time.monotonic() + delay
            self._sender.schedule(release_at, data, send_fn)
            self.sent_count += 1
            if duplicate:
                # 重复包同样排队，与原始包近乎同时释放，模拟链路 dup
                self._sender.schedule(release_at, data, send_fn)
                self.dup_count += 1
            return True

        # 无延迟路径：内联发送
        send_fn(data)
        self.sent_count += 1
        if duplicate:
            send_fn(data)
            self.dup_count += 1
        return True

    def apply_delay(self, size: int = 0) -> None:
        """只做 bandwidth + delay，不做 loss / dup。

        面向 TCP 这种流式可靠通道：TCP 语义下"应用层丢包"没有意义（内核会
        重传），但延迟是有效的。reference client 的 TCP 分支用它。
        """
        p = self.profile
        if p.delay_ms <= 0 and p.jitter_ms <= 0 and self._bucket is None:
            return
        if self._bucket is not None and size > 0:
            self.total_throttle_seconds += self._bucket.acquire(size)
        delay = self._sample_delay_seconds()
        if delay > 0:
            self.total_delay_seconds += delay
            time.sleep(delay)

    # -- 接收路径 --------------------------------------------------------

    def impair_recv(self, data: bytes) -> Optional[bytes]:
        """接收路径注入：**只做 reorder**（不做 loss / delay）。

        - 返回 `None`：本次不向上层交付（被 reorder 暂存、稍后随另一次
          调用释放）。
        - 返回 bytes：向上层交付的包，可能是当前包，也可能是先前暂存的
          旧包（这就是"乱序"）。

        **为何收侧不丢包/不加延迟**（对齐 netem）：netem.sh 用
        `tc qdisc ... root netem loss X%` 只整形 **egress**，回程包不受
        客户端侧 netem 影响。若收侧也丢包，往返存活率会变成 (1-X%)²，与
        netem 的 (1-X%) 不符（loss=5% 时 UDP 送达率会掉到 ~90% 而非 ~95%）；
        若收侧也加 delay，RTT 会变成 2×delay_ms。故 loss/delay 只在发送侧；
        reorder 留在收侧，让客户端直接观测乱序到达（不丢包）。
        """
        p = self.profile
        if p.is_passthrough():
            self.recv_count += 1
            return data

        # 乱序：hold-and-release 模型（收侧唯一的损伤；loss/delay 都在发送侧）
        #    - 触发（prob = reorder_pct）：把当前包压入 buf；若 buf 已有 ≥1
        #      个旧包，弹出最老的一个交付（形成"后到先发"的乱序）；否则
        #      本轮返回 None（当前包被暂存）。
        #    - 未触发：直接透传当前包，不动 buf（已暂存的旧包等到下一次
        #      触发时再弹出，或最后由 drain_reorder() 收尾）。
        #    不变量：total_in == total_out + len(buf)，reorder 不丢包。
        if p.reorder_pct > 0:
            if self._rand() * 100.0 < p.reorder_pct:
                self._reorder_buf.append(data)
                self.reorder_hold_count += 1
                if len(self._reorder_buf) > 1:
                    out = self._reorder_buf.pop(0)
                    self.reorder_release_count += 1
                    self.recv_count += 1
                    return out
                # 首次 hold：buf 里只有当前包，本轮无输出
                return None
            # 未触发乱序 → 透传当前包；buf 保持不变
            # （buf 超上限时兜底释放，防御极端配置）
            if len(self._reorder_buf) >= self._REORDER_BUF_MAX:
                out = self._reorder_buf.pop(0)
                self.reorder_release_count += 1
                self.recv_count += 1
                return out
            self.recv_count += 1
            return data

        self.recv_count += 1
        return data

    def drain_reorder(self) -> list[bytes]:
        """排空 reorder buffer（客户端关闭前调用，避免尾部包丢失）。"""
        out = list(self._reorder_buf)
        self._reorder_buf.clear()
        self.reorder_release_count += len(out)
        self.recv_count += len(out)
        return out

    # -- 统计 ------------------------------------------------------------

    def stats_snapshot(self) -> dict:
        """返回统计快照（可 JSON 序列化）。"""
        return {
            "profile": self.profile.name,
            "sent": self.sent_count,
            "dropped_send": self.dropped_send_count,
            "duplicated": self.dup_count,
            "received": self.recv_count,
            "dropped_recv": self.dropped_recv_count,
            "reorder_hold": self.reorder_hold_count,
            "reorder_release": self.reorder_release_count,
            "total_delay_ms": round(self.total_delay_seconds * 1000.0, 3),
            "total_throttle_ms": round(self.total_throttle_seconds * 1000.0, 3),
        }

    # -- 生命周期 --------------------------------------------------------

    def stop(self) -> None:
        """停止后台发送线程（flush 剩余待发包）。幂等，可重复调用。

        benchmark 客户端在 `session.close()` 里、关闭 socket **之前**调用，
        确保已入队的延迟包在 socket 仍可用时发出，不丢尾包。
        """
        if self._sender is not None:
            self._sender.stop()

    def close(self) -> None:
        """`stop()` 的别名，便于显式释放语义。"""
        self.stop()

    def __del__(self) -> None:  # pragma: no cover — 兜底，正常由 close() 触发
        try:
            self.stop()
        except Exception:  # noqa: BLE001
            pass


__all__ = [
    "ImpairmentProfile",
    "ImpairmentEngine",
    "TokenBucket",
    "PROFILES",
    "get_profile",
]
