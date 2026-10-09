# Demo 06 · AEAD 加密协商（C ABI，A/B 两轮对照）

只调用 **C ABI**（`ork_host_*` + `ork_client_*`），在同一个进程里跑两轮完整的
host + client 配对，用可观测证据回答一个问题：**加密到底是怎么打开的？**

```
Round A   服务端 enable_encryption = 1  →  OrkConnectionInfo.encryption_active == 1
Round B   服务端 enable_encryption = 0  →  OrkConnectionInfo.encryption_active == 0（对照组）
```

两轮都做 Reliable(KCP) + Unreliable(UDP) 双通道 25B 回显，并断言：

* 回显字节 **完全一致** —— 加解密对业务层 **完全透明**，payload 语义不变
* `OrkStats.crypto_fail_aead / replay / expired / malformed` **全为 0**
* Round A 的 `encryption_active` 必须为 1，Round B 必须为 0

## 密钥从哪来（这就是"bootstrap 期间协商 cipher"的含义）

客户端 TCP 连上后，服务端下发一帧 **Bootstrap V2（109B，opaque）**，里面带：

```
connectionId / sessionToken / c2sKey / s2cKey / bootstrapEpoch / flags（含加密位）
```

客户端据此建立 **ChaCha20-Poly1305-IETF** 的 `OrkCipher` 双向密钥；随后
**所有** UDP / KCP 数据帧都在传输层被逐帧 `Seal` / `Open`，业务层
（本文件的 `on_payload` 回调）看到的永远是明文字节。密钥有效期由
`bootstrap_ttl_sec` 控制（默认 300s），过期后 `crypto_fail_expired` 会开始计数。

## ★ 权威开关在服务端

客户端 `OrkClientConfig.enable_encryption` 只是 **意向**；实际是否加密以服务端
下发的 `BootstrapFrame.flags` 为准（见 `ork_client_abi.h` 的 `@note`）。
Round B 就是把服务端关掉做对照 —— 客户端两轮都填 `enable_encryption = 1`
也一样，`encryption_active` 会跟着服务端翻成 0。

## ★ Bootstrap 通道本身的安全 ≠ 数据通道的加密

本 demo（以及 C ABI Host 恒定使用的 `PlaintextDevelopment` 模式）里
**bootstrap 走明文 TCP**，所以 SDK 会被强制 bind 到 loopback，并在启动时打一条
**critical** 告警。这条告警只有 `ORK_LOG_LEVEL=off` 才会被抑制。
生产环境请用 C++ SDK（[demo 03](../03_cpp_host/)）配 `TlsServerAuthenticated`
+ TLS 1.3 证书链 / 私钥。

## 为什么是 `main.cpp` 而不是 `main.c`

Ork 的 C ABI 本身是纯 C11 的（[demo 01](../01_echo_server/) /
[demo 02](../02_echo_client/) 就是 `main.c`）。本 demo 用 `.cpp` **只是为了复用
`demos/common/demo_support.hpp` 里的 `PickFreePort()`**（需要 asio 才能跨平台地
同时探测 TCP + UDP 空闲端口）与几个时间小工具 —— **所有 Ork API 调用都是 C ABI**，
没有一个 C++ SDK 类型。

> ⚠️ 在 C++ 里 **不能** 用 `ORK_HOST_CONFIG_INIT` / `ORK_CLIENT_CONFIG_INIT`
> 这类 C99 复合字面量宏（工程设了 `CMAKE_CXX_EXTENSIONS OFF`，复合字面量非法）。
> 改成 `OrkHostConfig c{};` + 手工填 `struct_size`。

## 展示内容

1. `ork_host_get_connection_info()` 取 `OrkConnectionInfo.encryption_active` 与
   `.link`，作为 bootstrap 协商结果的服务端侧证据。
2. `OrkHostConfig.enable_encryption` 是唯一被改动的字段，其余两轮完全一致。
3. `on_payload` 回调里 **原样、同通道** 回显，客户端逐字节 `memcmp`。
4. `on_stats` 缓存四个 `crypto_fail_*` 计数器 + `bytes_in` / `bytes_out`。
   主循环在断言前会额外 pump ~1.3s，**确保 `OnStats` 至少落过一次**
   （约 1Hz），否则 `crypto_fail_*` 恒为 0 就没有意义了。
5. Unreliable 通道 150ms 一次、最多 8 次的节流重发（握手刚完成时 UDP 路由
   可能还没就绪）。
6. 两轮各自 `ork_host_create` / `ork_client_create`。上游 `EnsureLogger()` 用
   `std::call_once`，所以同进程多次 `create` **不会** 触发
   `spdlog::register_logger` 重名异常。

## 运行

```bash
./build/bin/06_encryption                    # 跑 A/B 两轮
./build/bin/06_encryption --timeout 20
./build/bin/06_encryption --rounds a         # 只跑加密轮
./build/bin/06_encryption --rounds b         # 只跑对照轮
ORK_LOG_LEVEL=debug ORK_LOG_FILE=/tmp/ork.log ./build/bin/06_encryption
```

端口是运行时向 OS 申请的临时端口（TCP + UDP 同时空闲），**不会** 和 9400 上的
其他 demo 抢端口，也不需要外部服务端。

典型输出（`ORK_LOG_LEVEL=error ./06_encryption --timeout 15`）：

```
Round A: [host] + connection 1  link=KCP  encryption_active=1  <<< bootstrap 协商结果
         [host] stats: aead=0 replay=0 expired=0 malformed=0 decode=0  bytes in/out=257/323  active=1
Round B: [host] + connection 1  link=KCP  encryption_active=0  <<< bootstrap 协商结果
         [host] stats: aead=0 replay=0 expired=0 malformed=0 decode=0  bytes in/out=482/582  active=1
  Round A 加密 enc=1  encryption_active=1 (期望 1) OK  link=KCP  echo R/U=OK/OK  cryptoFail=0  PASS
  Round B 对照 enc=0  encryption_active=0 (期望 0) OK  link=KCP  echo R/U=OK/OK  cryptoFail=0  PASS
 结论：encryption_active 随服务端 enable_encryption 翻转 (1 -> 0)，两轮 payload 均逐字节一致，crypto_fail_* 全 0。
PASS: C ABI encryption negotiation
```

> **`bytes in/out` 是进程级累计值**：Ork 的 Metrics 是单例，所以 Round B 的
> `482/582` 里包含 Round A 的 `257/323`。要按轮统计请自己做差分。
> 加密失败四计数器不受此影响（两轮都是 0）。

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 所请求的轮次全部符合预期 |
| `1` | 任一失败（create / start / connect 失败、超时、回显不一致、`encryption_active` 不符、`crypto_fail_*` 非 0） |
| `2` | 命令行参数非法 |

## 源码

[`main.cpp`](main.cpp) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`../common/demo_support.hpp`](../common/demo_support.hpp)
