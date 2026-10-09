# Demo 03 · C++ SDK 服务端（`SessionFramework` + `IHostListener`）

与 demo 01（纯 C ABI）功能等价，但走 **C++20 接入面**，多拿到两件关键能力：
可配 TLS 1.3 的安全 bootstrap，以及 `IConnectionRegistry` 的连接治理。

## 与 demo 01 的差别

| 维度 | 01 · C ABI Host | 03 · C++ SDK Host |
|------|-----------------|-------------------|
| 入口 | `ork_host_create` / `_start` / `_pump_once` | `SessionFramework::Create(cfg)` → `RegisterListener()` → `Start()` |
| 回调 | C 函数指针 + `void* user` | 继承 `ork::sdk::IHostListener`，四个 virtual |
| payload | `const uint8_t*` + `uint32_t` | `std::span<const uint8_t>` |
| 发送 | `ork_host_send` / `ork_host_broadcast` | `ork::sdk::ISender::Send` / `Broadcast(ConnectionSet, …)` |
| 连接治理 | `ork_host_get_connection_info` / `_connection_count` | `IConnectionRegistry`：`Count` / `TryGet` / `ForEach` / `Snapshot` / `Disconnect` |
| bootstrap 安全 | 恒 `PlaintextDevelopment`（且强制 loopback） | 可配 `TlsServerAuthenticated` + 证书链 / 私钥（生产安全默认） |
| 配置校验 | `create` 返回错误码 | `Create()` **抛 `std::invalid_argument` / `std::runtime_error`**，fail closed 无静默降级 |

## 展示内容

1. **`TransportConfig` 的正确填法** —— `demos/common/demo_support.hpp::MakeHostConfig()`
   给出与 `server/config/network.yaml` 对齐的模板：KCP on、100Hz KCP / 30Hz pump、
   `maxPacketSize` 1200、空闲超时 300s、AEAD on。
   注意 `kcpUpdateRate >= pumpRate` 是硬约束，`maxPacketSize ∈ [50, 1414]`。
2. **`EchoListener`（最小 `IHostListener`）** —— `OnPayload` 里
   `sender_.Send(connectionId, payload, channel)` 原样同通道回显。
   `ISender::Send` 内部只把 payload 拷进出站队列，返回后 `span` 即可失效。
3. **`OnStats` 的正确用法** —— 约 1Hz，**只缓存标量**，别在回调里做重活。
4. **`IConnectionRegistry` 的三个只读能力** —— 心跳里同时用 `Count()` 与
   `ForEach()` 数一遍，展示两条路径给出同一个数；`Snapshot()` 返回值语义的
   `std::vector<ConnectionView>`，可以安全带到回调外。
5. **干净退出顺序** —— `Stop()` → `RegisterListener(nullptr)` → 析构。
   **先摘监听再析构**，杜绝悬垂回调。

## 命名空间速查（很容易搞错）

| 全局命名空间 | `ork::sdk` | `ork::platform` |
|--------------|-----------|-----------------|
| `SessionFramework`、`TransportConfig`、`IConnectionRegistry`、`ConnectionView`、`TransportLinkState`、`TransportChannel`、`DisconnectReason`、`ConnectionId`、`LogLevel`、`Logger` | `IHostListener`、`ISender`、`BroadcastReport`、`ConnectionSet`、`Channel`、`OrkStats`、`SendResult`、`ToString` | `BootstrapSecurityMode`、`SendResult`（本体）、`ToString(SendResult)` |

> ⚠️ **显式命名空间限定会关闭 ADL**。`SendResult` 的 `ToString` 本体在
> `ork::platform`，`ork::sdk::ToString` 只是个 using 别名；写
> `ork::client::ToString(r)` 会编译失败（只匹配到 `ToString(ClientState)`）。
> 要么裸写 `ToString(r)` 让 ADL 生效，要么显式写 `ork::platform::ToString(r)`。

## 线程契约（见 `ork/sdk/host_listener.hpp`）

* 四个回调都在 core 内部 NetworkPump 的 asio strand 线程上触发
* 实现必须 **非阻塞**：不要在回调里做磁盘 IO / 长时间持锁
* ❌ **禁止**在回调里调 `SessionFramework::Stop()` 或析构框架（会 join 自己所在线程 → 死锁）
* ✅ `IConnectionRegistry::Disconnect()` 与 `ISender::Send()` / `Broadcast()` 在回调里安全
* `OnPayload` 的 `span` 只在本次回调内有效，要留存必须自己拷贝

## 运行

```bash
# 终端 1
ORK_LOG_LEVEL=warn ./build/bin/03_cpp_host                 # 监听 9400
./build/bin/03_cpp_host --port 9500
./build/bin/03_cpp_host --port 9500 --no-encryption

# 终端 2
./build/bin/04_cpp_client --port 9500 --count 3
```

> **日志走 stdout**：C++ 侧 `Logger::Init()` 挂的是 **stdout** 彩色 sink + 文件 sink，
> 所以 `2>/dev/null` **挡不住** SDK 日志。要纯净输出用 `ORK_LOG_LEVEL=error` 或 `off`；
> 要落文件用 `ORK_LOG_FILE=/tmp/ork.log`。明文 bootstrap 的 critical 告警只有
> `off` 才会被抑制。

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 收到 SIGINT / SIGTERM 后干净退出 |
| `1` | `SessionFramework::Create` / `Start` 抛异常（`what()` 会打印） |
| `2` | 命令行参数非法 |

## 源码

[`main.cpp`](main.cpp) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support.hpp`](../common/demo_support.hpp)
