# Ork SDK · C 示例工程

本目录是一个 **完全自包含** 的 C 集成示例：3 个 C 头、预编译合并静态库、
header-only 第三方依赖全部随包分发，构建 demo **不需要** `harmonia_ork` 源码仓，
也**不需要**任何内部 C++ 头。

五个 demo 全部是 **纯 C11**，只 include `ork/sdk/c/` 下的 3 个 C 头：

| # | 目录 | 接入面 | 一句话 |
|---|------|--------|--------|
| 01 | [`demos/01_echo_server`](demos/01_echo_server/) | C ABI Host（13 个 `ork_host_*`） | 最小回显服务端：create → callbacks → start → pump → stop |
| 02 | [`demos/02_echo_client`](demos/02_echo_client/) | C ABI Client（9 个 `ork_client_*`） | 最小回显客户端：v2 配置 96B、手动 `pump_once` 驱动、双通道校验 |
| 05 | [`demos/05_broadcast`](demos/05_broadcast/) | `ork_host_broadcast` + `ork_host_send` | 广播与扇出：全员广播 + 自维护连接表按 id 逐个发送 + N×2 矩阵 |
| 06 | [`demos/06_encryption`](demos/06_encryption/) | `ork_host_*` + `ork_client_*` | AEAD 密钥协商：A/B 两轮对照证明 `encryption_active` 随服务端开关翻转 |
| 07 | [`demos/07_tutorial`](demos/07_tutorial/) | C ABI 回调 | 真实 Host+Client 走读：on_connected → 应用层命令 → on_disconnected |

另有三个 CTest 用例（[`tests/`](tests/)），全部自包含、可 `ctest -j8` 并行。

---

## 目录结构

```
cpp/
├── CMakeLists.txt              顶层工程（C11 + CXX 链接驱动、ork_configure_demo()）
├── cmake/FindOrk.cmake         把 include/ + lib/ + thirdparty/ 装配成 Ork::Core
├── include/ork/sdk/c/          唯一的公开头目录：3 个 C 头
│   ├── ork_abi.h                   C ABI Host
│   ├── ork_client_abi.h            C ABI Client
│   └── ork_export.h                跨平台符号可见性
├── thirdparty/include/         随包分发的 header-only 依赖（asio / fmt / ikcp / …）
├── lib/macos-arm64/            合并库 libork_static.a + manifest.json + SHA256SUMS.txt
├── lib/windows-x64/            Windows 放置说明（归档未随仓提交）
├── demos/
│   ├── common/demo_support_c.h 纯 C11 demo / test 共用小工具（非 SDK 的一部分）
│   ├── 01_echo_server/         main.c + CMakeLists.txt + README.md
│   ├── …                       （每个 demo 三件套）
└── tests/                      三个 CTest 用例 + CMakeLists.txt
```

---

## 前置条件

| 项 | 要求 | 说明 |
|----|------|------|
| 平台 | **macOS arm64**（`lib/macos-arm64/` 已放交付单库）；`lib/windows-x64/` 目前只有放置说明 | 其他 triplet 需自行放入 `lib/<os>-<arch>/`，`FindOrk.cmake` 会自动按 `ORK_PLATFORM` 选取，也可 `-DORK_PLATFORM=…` 覆盖 |
| CMake | **≥ 3.20** | 工程用 `cmake_minimum_required(VERSION 3.20)`；CMake 4.x 已移除 < 3.5 的兼容层 |
| 编译器 | 支持 **C11**（+ C++ 链接驱动） | AppleClang 15+ / Clang 14+ / GCC 11+ / MSVC 19.30+。源文件全是 `.c`；链接走 C++ 驱动，因为归档内部是 C++ 实现 |
| OpenSSL | **可选** | 纯 C demo 不需要；只有 C++ 消费者 include `<asio/ssl.hpp>` 时才需要头。实现已并入单库，**不链接** `libssl` / `libcrypto` |
| libsodium | **不需要** | 单库形态已把它并进归档 |

`asio`、`fmt`、`spdlog`、`ikcp`、`concurrentqueue` 都已随包（header-only 或已编进静态库），
**不需要** gRPC / Protobuf / Abseil / yaml-cpp。

可用的 CMake 覆盖项：

```bash
-DCMAKE_BUILD_TYPE=Debug          # 默认 RelWithDebInfo
-DOPENSSL_ROOT_DIR=<prefix>       # 仅 C++ 消费者需要 OpenSSL 头时
-DORK_PLATFORM=macos-arm64        # 覆盖 triplet 自动探测
-DORK_PLAINTEXT_BOOTSTRAP=ON|OFF  # 默认跟随 manifest.json
```

---

## 构建

```bash
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

所有可执行文件统一落在 **`build/bin/`**（多配置生成器如 Xcode / VS 也一样）：

```
build/bin/
├── 01_echo_server   ├── 02_echo_client
├── 05_broadcast     ├── 06_encryption      ├── 07_tutorial
├── test_c_abi_host  ├── test_c_abi_client   └── test_c_roundtrip
```

仓库根目录也提供了快捷方式：

```bash
make test-cpp     # = cmake -B build && cmake --build build && ctest --output-on-failure
```

---

## 运行

### 方式 A：用随包的预编译服务端（`server/`）

```bash
bash ../server/start.sh                    # 起 embed_minimal，监听 9400（TCP + UDP/KCP 共用）
./build/bin/02_echo_client --port 9400     # C ABI 客户端
bash ../server/stop.sh
```

### 方式 B：自己起 demo 服务端（两个终端）

```bash
# 终端 1
ORK_LOG_LEVEL=warn ./build/bin/01_echo_server 9400      # 端口是位置参数

# 终端 2
./build/bin/02_echo_client --port 9400 --count 3
```

### 方式 C：单进程自证（不需要任何外部服务端）

`05_broadcast`、`06_encryption`、`07_tutorial` 会在自己进程里同时起 host 和
client，并且默认用运行时申请的临时端口：

```bash
./build/bin/05_broadcast --clients 3          # 3 个进程内客户端 + 广播扇出矩阵
./build/bin/06_encryption                     # A/B 两轮加密对照
./build/bin/07_tutorial                       # 回调与生命周期四步走读
```

三者都以退出码自证：`0` = 断言全过，`1` = 失败，`2` = 参数错误。

> **端口**：Ork 的 TCP bootstrap 与 UDP/KCP **共用同一个端口**。demo 默认 9400；
> 客户端 `udp_port` / `udpPort` 留 0 会回落到 **9401**，必然连不上——务必显式设成与
> `tcp_port` 相同的值。

---

## 测试

```bash
cd build && ctest --output-on-failure
```

| 用例 | 语言 | 覆盖 |
|------|------|------|
| `c_abi_host` | C11 | ABI 版本、NULL 句柄安全、`ork_host_create` 全部参数校验、start 前的拒绝语义、start / 重复 start、零连接下的 `get_stats` / `get_connection_info` / `disconnect` / `broadcast`（vacuous Ok）/ `send`、stop / 重复 stop、destroy 幂等 |
| `c_abi_client` | C11 | `sizeof(OrkClientConfig)==96`、v1(72B) 向后兼容、`struct_size` 精确白名单、NULL 句柄安全、**初始状态 Disconnected**、未连接 `send → NotConnected`、connect 到无人监听端口的异步失败路径、断开后可重连、destroy |
| `cpp_roundtrip` | C++20 | 同一进程内 `SessionFramework` host + `OrkClient`：Reliable(KCP) 与 Unreliable(UDP) 各 25B 探针逐字节回显一致、`IConnectionRegistry::Count()==1`、`ConnectionView.encryptionActive==true`、`linkState==Established`、`crypto_fail_*==0`、干净收尾不 hang |

三个用例都 **不依赖** `server/` 下的预编译服务端，需要端口的用例在运行时向 OS
申请临时端口（host 用例要求 TCP+UDP 同时空闲），因此不会和 9400 上手工跑的
demo 抢端口，也能安全并行。

---

## 链接依赖

`Ork::Core` 这个 IMPORTED 目标已经把整条链接闭包按正确顺序装好了，demo 只需
`target_link_libraries(x PRIVATE Ork::Core)`。

**交付形态是单库**，链接展开后只剩系统库：

```
libork_static.a        一个归档里含：Ork 传输核心 + platform + fmt + kcp
                       + libsodium + OpenSSL ssl/crypto
  → Threads::Threads   asio io_context + 内部 pump 线程
  → macOS: -framework CoreFoundation + resolv
                       （Windows：ws2_32 mswsock crypt32）
  → OpenSSL 头文件     仅 C++ 消费者编译时用（<asio/ssl.hpp>），不链接它的库
```

要点：

1. **只有一份归档**，不存在"哪个先链"的问题。
2. **`LINKER_LANGUAGE CXX` 是必须的**：`libork_static.a` 是合并了 C++ 实现的
   归档（asio、`std::span`、异常、RTTI、静态初始化）。纯 C 的 `main.c` 如果按 C
   链接，编译器驱动不会带上 C++ 运行时库，链接期会炸出成片的
   `__cxa_*` / `operator new` / `std::__1::*` undefined symbol。
   `ork_configure_demo()` 已对所有目标统一设置。
3. **编译宏必须与预编译库一致**：`FindOrk.cmake` 在 `Ork::Core` 上挂了
   `INTERFACE_COMPILE_DEFINITIONS`——`ORK_STATIC=1` 恒定（Windows 静态链接必需），
   `ORK_ALLOW_PLAINTEXT_BOOTSTRAP=1` 只在载荷确实是 ON 构建时才加
   （判定顺序：显式 `-DORK_PLAINTEXT_BOOTSTRAP` → 载荷 `manifest.json` → 默认 OFF）。
   宏不一致会让运行路径与本仓演示的行为对不上。**不要**自己手写
   `target_compile_definitions` 去覆盖它们，直接用 `Ork::Core`。
4. 不需要 `ASIO_*` 宏（纯 C demo 不 include asio；`Ork::Thirdparty` 仍指向随包的
   asio/fmt 头，供将来 C++ 消费者使用）。

---

## 日志与输出噪音（踩过的坑）

Ork 内部日志的落点取决于**走哪条接入面**，排障时很容易搞混：

| 场景 | 落点 | 静音方式 |
|------|------|----------|
| Logger 未 Init（C ABI Client：`OrkClientConfig` **没有** `log_path` 字段） | **stderr**，且所有级别不加过滤，还带 `harmonia_ork` 源码绝对路径，形如 `[ork][pre-init] [file:line] msg` | `./02_echo_client … 2>/dev/null` |
| C ABI Host（`OrkHostConfig.log_path` / `.log_level`） | 由这两个字段决定：`log_path=/dev/null` + `log_level=OrkLog_Off` 即完全静音 | 填字段 |

通用环境变量（`demos/common/demo_support_c.h` 与所有 C demo 都识别）：

```bash
ORK_LOG_FILE=/tmp/ork.log        # 内部日志文件（默认 /dev/null，即丢弃）
ORK_LOG_LEVEL=trace|debug|info|warn|error|off   # 默认 warn
```

> **注意**：明文 bootstrap（`PlaintextDevelopment`）会在启动时打一条
> **critical** 级别告警（"绑定被强制收敛到 loopback"之类）。这条告警只有在
> `ORK_LOG_LEVEL=off` 时才会被抑制；设成 `error` 仍会打印。这是有意的
> fail-loud 设计，不是 bug。

---

## 已知限制与说明

* **`lib/<triplet>/` 只接受一种载荷形态：合并包**。`FindOrk.cmake` 找不到
  `libork_static.a` / `ork_static.lib` 就直接在 configure 阶段报错，**不回落**
  分离库（旧的 `libork_core.a` + `libork_platform.a` 布局已随 C++ SDK 源码一并移除）。

  合并包把 fmt、kcp、libsodium、OpenSSL 全部并了进去，链接时只剩系统库
  （Windows：`ws2_32 mswsock crypt32`；macOS：CoreFoundation + resolv）。
  **不要**在这种形态下再链 `libssl.lib` / `sodium.lib`，重复引入没有收益。

  把包的 `manifest.json` 一起放进去，configure 就能打出版本与来源提交
  （`package : ork-static-sdk-0.2.0-darwin-arm64 — built from …`）；不放也能跑，
  只是 provenance 显示 unknown。
* **Windows 静态消费必须定义 `ORK_STATIC`**（`FindOrk.cmake` 已自动加到
  `INTERFACE_COMPILE_DEFINITIONS`）。少了它，`ork_export.h` 会把 C ABI 声明展开成
  `__declspec(dllimport)`，而 `dllimport` 符号无法从 `.lib` 归档解析，表现是链接期
  成片 `LNK2019` / `LNK4217`——不是库缺符号，是宏没对齐。
* **MSVC 运行时必须是 `/MD`（默认）**。Ork 归档用 `/MD` 编译，消费方若改 `/MT`
  会在链接期撞 `LNK2038: RuntimeLibrary 不匹配`。`CMakeLists.txt` 只对 Ork 自身
  强制了 `CMAKE_MSVC_RUNTIME_LIBRARY`，下游工程若自己覆盖了该变量，需要保持同值。
* **`ORK_ALLOW_PLAINTEXT_BOOTSTRAP` 必须与归档的构建选项一致**。这个开关决定
  库内是否编入明文 bootstrap 路径。`FindOrk.cmake` 的判定顺序是：显式
  `-DORK_PLAINTEXT_BOOTSTRAP=ON|OFF` → 载荷里的 `manifest.json` → 默认 OFF。
  **交付到生产的正式包是 OFF（TLS-only）**，拿它跑本仓 demo 会在
  `ork_host_create` 处 fail closed —— 那是预期的安全默认，不是包坏了。C ABI 目前
  没有 TLS 配置字段，所以本仓 `lib/macos-arm64/` 放的是 **dev 包**
  （`plaintext_bootstrap_compiled=true`，仅 loopback），供 demo 本地运行。
* **C ABI Host 恒定使用 `PlaintextDevelopment` bootstrap**，因此 SDK 会把监听
  地址强制收敛到 loopback。生产接入请使用 C++ 的 `SessionFramework` +
  `TlsServerAuthenticated` + TLS 1.3 证书链 / 私钥（本仓只演示 C ABI）。
* **`ork_host_pump_once()` 是 1ms 协作式让出的桩**：内部恒由独立 pump 线程驱动 IO，
  这个函数**不会**驱动网络循环。真正「调用方独占驱动」的是
  `ork_client_pump_once()`（`io.poll()` + `io.restart()`）。
* **`ork_host_send()` 到不存在的连接返回 `OrkSend_BadState`，不是 `NotConnected`**：
  `Sender::Send` 把发送委托给 `TrySend(bool)`，`false` 无法细分
  NotConnected / QueueFull / CipherFailed / NoRoute，于是保守映射为 `BadState`。
  C ABI 的 `OrkSend_NotConnected` 实际上只会从 `ork_client_send()`（客户端未
  Connected）产生。
* **`OrkStats.bytes_*` 是进程级累计**（Metrics 单例），同一进程里跑多轮
  （demo 06 的 A/B 两轮）时后一轮的数字会包含前一轮。要按轮统计请自己差分。

---

## 相关文档

* 每个 demo 目录下的 `README.md` —— 展示内容、运行命令、退出码、关键源码位置
* [`../server/README.md`](../server/README.md) —— 预编译服务端
* [`../python/README.md`](../python/README.md) —— Python 接入面（ctypes + Cython KCP）
