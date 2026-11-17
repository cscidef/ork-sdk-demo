# Demo 05 · 广播与扇出（`ork_host_broadcast` + 按连接 ID 逐个 `ork_host_send`）

C ABI 只有两个发送原语。本 demo 把它们的用法和边界讲透，并把"到底发给了谁"
打成一张可断言的整数矩阵。

| 原语 | 语义 | 宿主需要知道谁在线吗 |
|------|------|----------------------|
| `ork_host_broadcast(host, data, size, channel)` | 向**当前所有活跃连接**扇出同一份字节 | 不需要 |
| `ork_host_send(host, connection_id, data, size, channel)` | 向**指定连接**发送 | 需要自己维护连接表 |

## ⚠️ C ABI 没有的东西（本 demo 的核心知识点）

* **没有 `ConnectionSet::Where/Of`** —— C ABI 不存在"谓词集合"这种对象。
  所谓"只发给一部分人"，就是宿主遍历自己的 connection_id 表、跳过不想发的，
  然后逐条 `ork_host_send`。
* **没有 `BroadcastReport`** —— `broadcast` 与 `send` 都只返回一个
  `OrkSendResult`。发了几条、几条成功，**必须宿主自己统计**。本 demo 打了
  三个计数器：`broadcast(ok/calls)`、`filtered(ok/fail/sends)`。
* **没有枚举连接列表的函数** —— `ork_host_connection_count()` 只给数量，
  `ork_host_get_connection_info()` 需要已知 id。因此宿主**必须**在
  `on_connected` / `on_disconnected` 里自己维护连接表。这是本 demo 的首要示范。

## 扇出矩阵（断言口径）

进程内起 N 个 C ABI 客户端，每个连上后发一帧探针（marker=`ORKBCSTP`）。
宿主收到探针后做两件事：

| marker（8B） | 动作 | N 个客户端时每个客户端应收 |
|--------------|------|---------------------------|
| `ORKBCSTA` | `ork_host_broadcast` 全员广播 | **N**（含探针来源自己） |
| `ORKBCSTF` | 遍历连接表、跳过来源、逐个 `ork_host_send` | **N − 1** |

N 个客户端各触发一轮，共 N 轮，于是每个客户端累计：

```
All      = N          （每轮 1 条 × N 轮）
Filtered = N - 1      （其余 N-1 轮里它都不是来源）
```

## 防广播风暴

只有 `ORKBCSTP` 探针帧才触发放大。广播出去的 `ORKBCSTA` / `ORKBCSTF` 回到
宿主的 `on_payload` 时被 marker 门禁挡掉，否则会无限递归放大。

## 连接表怎么维护

`on_connected` / `on_disconnected` 只在 SDK 内部 pump 线程触发，因此结构变更
不需要锁；主线程只读快照，用 `_Atomic` 元素 + `atomic_load` 拿一致值。
`connection_id` 由 SDK 分配、不会为 0，所以槽位值 0 就是"空"。

## 运行

```bash
# 自证模式（默认 3 客户端）
./build/bin/05_broadcast --clients 3
./build/bin/05_broadcast --clients 5 --timeout 40
./build/bin/05_broadcast --no-encryption

# 纯服务端模式：等外部客户端连进来，Ctrl+C 退出
./build/bin/05_broadcast --clients 0 --port 9500
# 另开终端
./build/bin/02_echo_client --port 9500 --count 3
```

典型输出（`--clients 3`）：

```
[host] broadcast(All)     25 B via Reliable -> Ok  (在线 3)
[host] filtered(origin=2) attempted=2 ok=2 fail=0  (在线 3)
...
        client   All(N=3)  Filtered(N-1=2)   verdict
        #0       3         2                 OK
        #1       3         2                 OK
        #2       3         2                 OK
[host] 汇总: probes=3 rounds=3 broadcast(ok/calls)=3/3 filtered(ok/fail/sends)=6/0/6  clients=3 settled=yes
PASS: C ABI broadcast fanout (All + filtered per-connection send)
```

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 自证模式：矩阵与宿主计数全部符合期望；纯服务端模式：Ctrl+C 干净退出 |
| `1` | 矩阵不符 / 计数不符 / 超时 |
| `2` | 命令行参数非法 |

## 注意

`ork_host_send` 的返回值里 `OrkSend_NotConnected` 是正常竞态——你遍历连接表时
对方可能刚好断开。本 demo 把它计入 `fail` 但不判失败；矩阵只对**收到的帧数**
做硬断言。

## 源码

[`main.c`](main.c) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support_c.h`](../common/demo_support_c.h)
