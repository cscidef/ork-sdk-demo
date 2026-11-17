# Demo 07 · C ABI 回调与生命周期教程（真实 Host + Client）

这个 demo 回答一个问题：**业务方要实现的回调长什么样，事件以什么顺序到达，
我在里面能做什么。**

与 01/02 的区别：01/02 演示"怎么把字节收发起来"，本 demo 用一条**真实**的
最小连接（同进程 host + client，真的走 TCP bootstrap → UDP 握手 → KCP），
把三个回调按顺序走一遍，每一步都停下来解释"Ork 此刻做了什么、业务方该做什么"。

> **不是 Mock。** C ABI 无法替换 `OrkHost` 的实现，所以"模拟回调"只会教出
> 错误的线程模型和生命周期预期。这里宁可真起一条连接。

## 四步教学流程

| 步骤 | 观察点 | Ork 做了什么 | 业务方该做什么 |
|------|--------|--------------|----------------|
| 1 | `on_connected` | TCP 握手 → 下发 BootstrapFrame → UDP 握手 → KCP ACK 全部完成，`connectionId` 已分配 | 建会话、发欢迎包、记审计 |
| 2 | 应用层命令 `PING` → `PONG` | 只搬运字节，**完全不理解 "PING" 的含义** | 在 `on_payload` 里自己解析命令、决定回什么 |
| 3 | 可靠 / 不可靠通道 | `Reliable` 走 KCP（有序可靠）；`Unreliable` 走裸 UDP（可能丢、不保序） | 按业务重要性选通道；不可靠通道自己容错 |
| 4 | `on_disconnected` | 宿主调用 `ork_host_disconnect` 后立即回收连接 | 清理会话；注意客户端感知是**异步**的（见下） |

## 线程模型（最容易踩坑的地方）

| 位置 | 在哪个线程 | 能做 | 绝对不能做 |
|------|-----------|------|-----------|
| Host 回调 | SDK 内部 pump 线程 | `send` / `broadcast` / `disconnect` | `stop` / `destroy`（会 join 自己 → 死锁） |
| Client 回调 | `ork_client_pump_once()` / `disconnect()` 的调用栈 | `send` / `disconnect` | `destroy`（析构正在执行的调用栈） |
| `main` | 主线程 | `stop` / `destroy` / 收尾 | 无 |

## 一个必须知道的真实行为

宿主 `ork_host_disconnect` 是**立即生效**的（`connection_count()` 当场归零，
host 侧 `on_disconnected` 收到 `Kicked`）。但**被踢的客户端不会立刻收到
`on_disconnected`** —— C ABI 不向被踢方传递原因，客户端要靠协议超时或自己的
心跳/重连策略发现。本 demo 在 Step 4 明确打印这一点，不做"立刻收到"的假断言。

## 运行

```bash
./build/bin/07_tutorial
./build/bin/07_tutorial --timeout 15 --no-encryption
```

典型输出：

```
 Step 2 · 应用层命令 PING → PONG（Ork 不理解 PING）
[client] -> "PING" via Reliable  send=Ok
[host] on_payload(4 B, channel=Reliable): "PING"
       └ 业务识别为 PING → 回 PONG（send=Ok）
[client] on_payload(4 B, channel=Reliable): "PONG"
 [观察] 收到 PONG —— 命令语义完全由业务两侧约定，Ork 只搬运字节
...
   on_connected      client=1  host=1
   on_payload        client=3  host=4（PING/ECHO/QUIT 各一条）
   on_disconnected   host=1 reason=Kicked  |  client=0（异步感知）
PASS: C ABI callbacks & lifecycle
```

## 和其他 demo 的关系

- 想看最小收发 → [01_echo_server](../01_echo_server) / [02_echo_client](../02_echo_client)
- 想看广播与按连接筛选 → [05_broadcast](../05_broadcast)
- 想看加密协商的证据 → [06_encryption](../06_encryption)

## 源码

[`main.c`](main.c) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support_c.h`](../common/demo_support_c.h)
