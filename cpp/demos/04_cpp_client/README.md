# Demo 04 · C++ SDK 客户端（`OrkClient` + `IClientListener`）

与 demo 02（纯 C ABI）功能等价，但走 **C++20 接入面**：类型安全的
`ClientConfig` / `ClientState` / `SendResult`，`std::span` 载荷，以及
可以打印完整状态机迁移的监听器。

## 展示内容

| 项 | 说明 |
|----|------|
| `ClientConfig` | `demos/common/demo_support.hpp::MakeClientConfig()` 给出模板。★ `tcpPort` 与 `udpPort` **必须相同**：`ClientConfig::udpPort` 默认是 **9401**，留默认必然连不上共用端口的服务端 |
| `securityMode` | `PlaintextDevelopment` 以匹配 `ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON` 的服务端；生产环境用 `TlsServerAuthenticated` + `caFile` + `expectedServerName` |
| 构造顺序 | `OrkClient` 持 **`asio::io_context&` 引用**、`ClientConfig` **按值** 拷贝。所以 `io` 必须声明在 `client` 之前（析构顺序自然相反），否则悬垂引用 |
| 状态机打印 | `Disconnected → Connecting → Bootstrapping → Handshaking → Connected`，每次迁移打一行 |
| 双通道探针 | Reliable(KCP) 发 `--count` 轮；Unreliable(UDP) 最多重试 8 次、每次间隔 150ms（握手刚完成时 UDP 路由可能还没就绪），收到回显即停 |
| 往返计时 | 25B 帧尾部 8B 是发送时的单调纳秒时间戳，回显里原样带回来 → 直接算 RTT |
| 干净退出 | `client.Disconnect()` → 继续 `PumpOnce()` 直到状态回 `Disconnected` → 析构 |

## 手动驱动：客户端没有内部线程

`OrkClient` **不** 起任何线程，所有 IO 与回调都由调用方的
`PumpOnce()` 驱动：

```cpp
while (!done) {
    client.PumpOnce();      // = io.poll() + io.restart()，非阻塞
    // …检查状态、补发探针…
    SleepMs(5);
}
```

因此回调（`OnConnected` / `OnPayload` / `OnDisconnected`）都在
**调用 `PumpOnce()` / `Disconnect()` 的那个线程的调用栈内** 触发：

* 回调里的普通成员变量不需要 atomic（本 demo 的 `DemoListener` 就是这么写的）
* `OnConnected` 触发时状态**已经是** `Connected`、KCP 会话也已建好，
  所以 **在 `OnConnected` 里直接 `client.Send()` 是安全的**
* ❌ 回调里 **不能** 析构 `OrkClient`（正在被析构的对象会在回调返回后继续被使用）

## `SendResult` 与 `ToString` 的坑

```cpp
const ork::client::SendResult r = client.Send(payload, TransportChannel::Reliable);
std::printf("send=%s\n", std::string(ork::platform::ToString(r)).c_str());
```

* `ork::client::SendResult` 是 `ork::platform::SendResult` 的别名
* `ToString(SendResult)` 的**本体在 `ork::platform`**，返回 `std::string_view`
  （静态存储，不分配内存）
* ⚠️ **显式命名空间限定会关闭 ADL**。写 `ork::client::ToString(r)` 编译失败：
  该命名空间里只有 `ToString(ClientState)`，于是报
  `cannot initialize a parameter of type 'ClientState' with an lvalue of type 'const SendResult'`。
  要么裸写 `ToString(r)` 让 ADL 生效，要么像上面那样显式写 `ork::platform::ToString(r)`。
* `TransportChannel` 在 **全局命名空间**（`ork/platform/data/transport_channel.hpp`
  直接在全局作用域定义），写 `ork::client::TransportChannel` 同样编译失败。

## 运行

```bash
# 服务端（任选其一）
bash ../server/start.sh
ORK_LOG_LEVEL=warn ./build/bin/03_cpp_host --port 9400

# 客户端
./build/bin/04_cpp_client                                # 连 127.0.0.1:9400，1 轮
./build/bin/04_cpp_client --port 9500 --count 3
./build/bin/04_cpp_client --timeout 20 --no-encryption
```

典型输出：

```
[client] state: Disconnected -> Connecting
[client] state: Connecting -> Bootstrapping
[client] state: Bootstrapping -> Handshaking
[client] state: Handshaking -> Connected
[client] -> Reliable(KCP)   25 B round=1 send=Ok
[client] -> Unreliable(UDP) 25 B attempt=1 send=Ok
[client] OnPayload Unreliable echo OK (25 B, rtt~4962 us)
[client] OnPayload Reliable   echo OK (25 B, rtt~44609 us)
 Reliable(KCP)   echo : OK (1/1 rounds)
 Unreliable(UDP) echo : OK (1 attempts)
PASS: C++ SDK echo roundtrip
```

> **日志走 stdout**：C++ 侧 `Logger::Init()` 挂的是 **stdout** sink，
> `2>/dev/null` 挡不住 SDK 日志。要纯净输出用 `ORK_LOG_LEVEL=error` 或 `off`。

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 连接建立 + 所有 Reliable 轮次 + 至少一次 Unreliable 回显逐字节一致 |
| `1` | 连接失败 / 超时 / 回显不一致 |
| `2` | 命令行参数非法 |

## 源码

[`main.cpp`](main.cpp) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support.hpp`](../common/demo_support.hpp)
