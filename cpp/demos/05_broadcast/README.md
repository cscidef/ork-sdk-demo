# Demo 05 · 广播扇出（`ISender::Broadcast` + `ConnectionSet` + `BroadcastReport`）

服务端在收到任意一个客户端的探针帧时，用 **三种不同的 `ConnectionSet` 形态**
各广播一次，然后把「谁收到了什么」变成一张可断言的整数矩阵 —— 让
`Broadcast` 的语义不再靠读文档猜。

## 三种 `ConnectionSet` 与三种 marker

| marker（8B） | `ConnectionSet` | 语义 | N 个客户端时每个客户端应收 |
|--------------|-----------------|------|---------------------------|
| `ORKBCSTA` | `All()` | 全部连接 | **N**（含发送者自己） |
| `ORKBCSTW` | `Where([](ConnectionId id){ return id != origin; })` | 谓词过滤，排除来源 | **N − 1** |
| `ORKBCSTO` | `Of(ids)` | 显式 ID 列表（这里取 `registry.Snapshot()` 全量） | **N** |

客户端 → 服务端的探针帧用 `ORKDEMO1`。**只有探针帧才会被放大广播**
（`IsProbeFrame()` 门禁），否则广播回来的帧又会触发下一次广播 → 无限风暴。

## `BroadcastReport` 的精确口径（本 demo 的核心知识点）

```cpp
struct BroadcastReport {
    uint32_t attempted;            // 尝试发送的连接数
    uint32_t sent;                 // TrySend 成功
    uint32_t failedNotConnected;   // 连接不存在 / 已 Disconnected
    uint32_t failedTooLarge;       // payload 超过 maxPacketSize（整批只做一次检查）
    uint32_t failedQueueFull;      // 出站队列满
    uint32_t failedCipher;         // AEAD Seal 失败
    bool IsConsistent() const;     // attempted == 上面五项之和
};
```

⚠️ **`attempted` 的口径随形态不同**（见上游 `sender.cpp`）：

| 形态 | `attempted` | 对 `Disconnected` 连接的处理 |
|------|-------------|------------------------------|
| `All()` | 传输层**全部**连接数（含 `Disconnected`） | 每条都 `attempted++`，然后 `failedNotConnected++` |
| `Of(ids)` | `ids.size()` | 发不出去 → `failedNotConnected++` |
| `Where(pred)` | **跳过** `Disconnected`，不计入 `attempted` | 谓词为真且非 Disconnected 才计数 |

另外几条边界：

* **空 payload** → 直接返回全零报告（不遍历任何连接）
* **超限 payload** → `attempted = 1`、`failedTooLarge = 1`，并 `IncDropsTooLarge()`
* C ABI 侧 `ork_host_broadcast` 把报告折叠成单个 `OrkSendResult`：
  `failedTooLarge>0 → TooLarge`；`attempted==0 → Ok`（vacuous，零连接广播算成功）；
  `sent>0 → Ok`；`failedQueueFull>=attempted → QueueFull`；否则 `NotConnected`

**所以本 demo 只对 `sent` 与 `IsConsistent()` 做硬断言，`attempted` 仅打印展示** ——
否则「刚好有客户端断开」这种正常竞态会造成假失败。

## 展示内容

1. `BroadcastListener::OnPayload` 在 **回调内**（core pump 线程）连续做三次
   `Broadcast`，证明 `ISender::Broadcast` 在回调里调用是安全的。
2. 用 `registry.Snapshot()` 组装 `Of(ids)` 的 ID 列表 —— `Snapshot()` 只是
   `ForEach` 的值拷贝，回调内调用安全。
3. 进程内起 N 个 `ork::client::OrkClient`（各自独立 `io_context` + listener）齐射探针，
   把「扇出是否正确」变成 N×3 的收帧矩阵。
4. 汇总服务端累计的 `BroadcastReport`：`rounds` / `sent(All)` / `sent(Where)` /
   `sent(Of)` / `inconsistent` / `cryptoFail` / `drops`。

期望值（N 个客户端，每个客户端触发一轮）：

```
sent(All)   == N * N
sent(Where) == N * (N - 1)
sent(Of)    == N * N
inconsistent == 0     cryptoFail == 0     drops == 0
```

5. `ClientGuard` RAII：任何返回路径都先 `Disconnect()` 再 pump 20×5ms，
   最后才 `clear()` 客户端容器。**全局容器的析构必须早于 `SessionFramework`**，
   否则客户端会在框架已析构后继续收发。

## 运行

```bash
# 自证模式（默认）：进程内同时起 host + 3 个 client
./build/bin/05_broadcast --clients 3
./build/bin/05_broadcast --clients 5 --timeout 40
./build/bin/05_broadcast --no-encryption

# 纯服务端模式：等外部客户端连进来，Ctrl+C 退出
./build/bin/05_broadcast --clients 0 --port 9500
# 另开终端
./build/bin/04_cpp_client --port 9500
```

`--port` 默认 9400。自证模式跑完即以退出码给出结论，不需要人工读输出。

典型输出（`--clients 3`）：

```
[host]     All()   attempted=3 sent=3 notConn=0 tooLarge=0 queueFull=0 cipher=0  consistent
[host]     Where() attempted=2 sent=2 notConn=0 tooLarge=0 queueFull=0 cipher=0  consistent
[host]     Of()    attempted=3 sent=3 notConn=0 tooLarge=0 queueFull=0 cipher=0  consistent
        client   All()     Where()   Of()      其他
        #0       3         2         3         0       OK
        #1       3         2         3         0       OK
        #2       3         2         3         0       OK
[test] 服务端 BroadcastReport 汇总: rounds=3 sent(All)=9 sent(Where)=6 sent(Of)=9 inconsistent=0 cryptoFail=0 drops=0
 clients=3  settled=yes  clientMatrix=OK  serverCounters=OK
PASS: ISender::Broadcast fanout (All / Where / Of)
```

> **日志走 stdout**：`2>/dev/null` 挡不住；要纯净输出用 `ORK_LOG_LEVEL=error` 或 `off`。
> 明文 bootstrap 的 critical 告警只有 `off` 才会被抑制。

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 自证模式：收帧矩阵与全部服务端计数都符合期望；纯服务端模式：Ctrl+C 干净退出 |
| `1` | 矩阵不符 / 计数不符 / 超时 / 异常 |
| `2` | 命令行参数非法 |

## 源码

[`main.cpp`](main.cpp) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support.hpp`](../common/demo_support.hpp)
