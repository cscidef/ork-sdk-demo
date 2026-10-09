# Ork SDK · C / C++ 示例工程

本目录是一个 **完全自包含** 的 C / C++ 集成示例：头文件、预编译静态库、
header-only 第三方依赖全部随包分发，构建 demo **不需要** `harmonia_ork` 源码仓。

六个 demo 覆盖 Ork 的两条接入面：

| # | 目录 | 语言 | 接入面 | 一句话 |
|---|------|------|--------|--------|
| 01 | [`demos/01_echo_server`](demos/01_echo_server/) | 纯 C11 | C ABI Host（13 个 `ork_host_*`） | 最小回显服务端：create → callbacks → start → pump → stop |
| 02 | [`demos/02_echo_client`](demos/02_echo_client/) | 纯 C11 | C ABI Client（9 个 `ork_client_*`） | 最小回显客户端：v2 配置 96B、手动 `pump_once` 驱动、双通道校验 |
| 03 | [`demos/03_cpp_host`](demos/03_cpp_host/) | C++20 | `SessionFramework` + `IHostListener` | C++ 服务端：`ISender` 回显 + `IConnectionRegistry` 治理 |
| 04 | [`demos/04_cpp_client`](demos/04_cpp_client/) | C++20 | `OrkClient` + `IClientListener` | C++ 客户端：完整状态机迁移 + Reliable/Unreliable 往返计时 |
| 05 | [`demos/05_broadcast`](demos/05_broadcast/) | C++20 | `ISender::Broadcast` | 广播扇出：`All()` / `Where()` / `Of()` 三种 `ConnectionSet` 形态 + `BroadcastReport` 自证 |
| 06 | [`demos/06_encryption`](demos/06_encryption/) | C ABI（.cpp 编译单元） | `ork_host_*` + `ork_client_*` | AEAD 密钥协商：A/B 两轮对照证明 `encryption_active` 随服务端开关翻转且收发透明 |

另有三个 CTest 用例（[`tests/`](tests/)），全部自包含、可 `ctest -j8` 并行。

---

## 目录结构

```
cpp/
├── CMakeLists.txt              顶层工程（C11 / C++20、ork_configure_demo()）
├── cmake/FindOrk.cmake         把 include/ + lib/ + thirdparty/ 装配成 Ork::Core
├── include/ork/                Ork SDK 公开头文件
│   ├── sdk/c/ork_abi.h             C ABI Host
│   ├── sdk/c/ork_client_abi.h      C ABI Client
│   ├── sdk/…                       C++ Host SDK（session_interface / host_listener / sender / …）
│   ├── client/…                    C++ Client SDK（ork_client / client_config）
│   └── platform|transport|codec|crypto|connection/…   被上面两组头连带依赖
├── thirdparty/include/         随包分发的 header-only 依赖（asio / fmt / spdlog / ikcp / …）
├── lib/macos-arm64/            预编译静态库（见「链接依赖」）
├── demos/
│   ├── common/demo_support.hpp C++ demo / test 共用小工具（非 SDK 的一部分）
│   ├── 01_echo_server/         main.c   + CMakeLists.txt + README.md
│   ├── …                       （每个 demo 三件套）
└── tests/                      三个 CTest 用例 + CMakeLists.txt
```

---

## 前置条件

| 项 | 要求 | 说明 |
|----|------|------|
| 平台 | **macOS arm64**（当前 `lib/` 只有 `macos-arm64`） | 其他 triplet 需另行放置 `lib/<os>-<arch>/`，`FindOrk.cmake` 会自动按 `ORK_PLATFORM` 选取，也可 `-DORK_PLATFORM=…` 覆盖 |
| CMake | **≥ 3.20** | 工程用 `cmake_minimum_required(VERSION 3.20)`；CMake 4.x 已移除 < 3.5 的兼容层 |
| 编译器 | 支持 **C++20** 与 **C11** | AppleClang 15+ / Clang 14+ / GCC 11+ / MSVC 19.30+。SDK 公开头用到 `std::span`、`<source_location>`、嵌套命名空间 |
| OpenSSL | **3.x**（开发头 + 库） | `brew install openssl@3`（keg-only，`FindOrk.cmake` 已内置 Homebrew 路径提示）／`apt install libssl-dev` |
| libsodium | 任意近期版本 | `brew install libsodium`／`apt install libsodium-dev`；也可 `-DSODIUM_ROOT=<prefix>` |

`asio`、`fmt`、`spdlog`、`ikcp`、`concurrentqueue` 都已随包（header-only 或已编进静态库），
**不需要** gRPC / Protobuf / Abseil。

可用的 CMake 覆盖项：

```bash
-DCMAKE_BUILD_TYPE=Debug          # 默认 RelWithDebInfo
-DOPENSSL_ROOT_DIR=<prefix>       # OpenSSL 找不到时
-DSODIUM_ROOT=<prefix>            # libsodium 找不到时
-DORK_PLATFORM=macos-arm64        # 覆盖 triplet 自动探测
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
├── 03_cpp_host      ├── 04_cpp_client
├── 05_broadcast     ├── 06_encryption
├── test_c_abi_host  ├── test_c_abi_client   └── test_cpp_roundtrip
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
./build/bin/04_cpp_client  --port 9400     # C++ 客户端
bash ../server/stop.sh
```

### 方式 B：自己起 demo 服务端（两个终端）

```bash
# 终端 1
ORK_LOG_LEVEL=warn ./build/bin/01_echo_server 9400          # 纯 C（端口是位置参数）
# 或
ORK_LOG_LEVEL=warn ./build/bin/03_cpp_host   --port 9400    # C++

# 终端 2
./build/bin/02_echo_client --port 9400
./build/bin/04_cpp_client  --port 9400 --count 3
```

### 方式 C：单进程自证（不需要任何外部服务端）

`05_broadcast` 与 `06_encryption` 会在自己进程里同时起 host 和 client，
并且默认用运行时申请的临时端口：

```bash
./build/bin/05_broadcast --clients 3          # 3 个进程内客户端 + 广播扇出矩阵
./build/bin/06_encryption                     # A/B 两轮加密对照
```

两者都以退出码自证：`0` = 断言全过，`1` = 失败，`2` = 参数错误。

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
`target_link_libraries(x PRIVATE Ork::Core)`。展开后是：

```
libork_core.a          Ork SDK 核心（session framework / transport / codec / crypto）
  → libork_platform.a  平台层（logger / metrics / config / bootstrap security）
  → libkcp.a           ikcp（Reliable 通道的 ARQ）
  → libfmt.a           格式化（spdlog 的依赖）
  → libyaml-cpp.a      NetworkConfig 等 YAML 解析
  → OpenSSL::SSL / OpenSSL::Crypto    TLS 1.3 + ChaCha20-Poly1305 底座
  → libsodium                         AEAD / HKDF / HMAC
  → Threads::Threads                  asio io_context + 内部 pump 线程
  → macOS: -framework CoreFoundation + resolv
```

要点：

1. **顺序有意义**：静态库按「依赖者在前」排列，`ork_core` → `ork_platform` → thirdparty。
2. **`LINKER_LANGUAGE CXX` 是必须的**：`libork_core.a` / `libork_platform.a` 是 C++ 归档
   （asio、`std::span`、异常、RTTI、静态初始化）。纯 C 的 `main.c` 如果按 C 链接，
   编译器驱动不会带上 C++ 运行时库，链接期会炸出成片的
   `__cxa_*` / `operator new` / `std::__1::*` undefined symbol。
   `ork_configure_demo()` 已对所有目标（含两个纯 C demo）统一设置。
3. **编译宏必须与预编译库一致**：`FindOrk.cmake` 在 `Ork::Core` 上挂了
   `INTERFACE_COMPILE_DEFINITIONS`，包含 `ORK_HAVE_TLS=1` 与
   `ORK_ALLOW_PLAINTEXT_BOOTSTRAP=1`。宏不一致会改变 `OrkClient` 的成员布局
   → ODR 破坏 → 运行期诡异崩溃。**不要**自己手写 `target_compile_definitions`
   去覆盖它们，直接用 `Ork::Core`。
4. 不需要 `ASIO_*` 宏（用 `#include <asio.hpp>` 的非 standalone-Boost 形态，随包头已配好）。

---

## 日志与输出噪音（踩过的坑）

Ork 内部日志的落点取决于**走哪条接入面**，排障时很容易搞混：

| 场景 | 落点 | 静音方式 |
|------|------|----------|
| Logger 未 Init（C ABI Client：`OrkClientConfig` **没有** `log_path` 字段） | **stderr**，且所有级别不加过滤，还带 `harmonia_ork` 源码绝对路径，形如 `[ork][pre-init] [file:line] msg` | `./02_echo_client … 2>/dev/null` |
| C ABI Host（`OrkHostConfig.log_path` / `.log_level`） | 由这两个字段决定：`log_path=/dev/null` + `log_level=OrkLog_Off` 即完全静音 | 填字段 |
| C++ SDK（demo 03/04/05、`test_cpp_roundtrip`） | `Logger::Init()` 挂 **stdout** 彩色 sink + 文件 sink，级别过滤生效 | `ORK_LOG_LEVEL=error` 或 `off`；`2>/dev/null` **挡不住** |

通用环境变量（`demos/common/demo_support.hpp` 与 C ABI demo 都识别）：

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

* **`lib/` 目前只有 `macos-arm64`**。要跑 Linux / Windows，需要先在
  `harmonia_ork` 侧为对应 triplet 构建并把 `.a` / `.lib` 放进 `lib/<os>-<arch>/`。
* **C ABI Host 恒定使用 `PlaintextDevelopment` bootstrap**，因此 SDK 会把监听
  地址强制收敛到 loopback。生产环境请用 C++ SDK（demo 03）配
  `TlsServerAuthenticated` + TLS 1.3 证书链 / 私钥。
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
* **`BroadcastReport.attempted` 的口径随 `ConnectionSet` 形态不同**：
  `All()` 遍历传输层全部连接（含 `Disconnected`，每条都 `attempted++`），
  `Where()` 直接跳过 `Disconnected`（不计 `attempted`），
  `Of()` 则 `attempted == ids.size()`。因此跨形态比较 `attempted` 没有意义；
  可靠的可断言量是 `sent` 与 `IsConsistent()`（`attempted == 五个计数之和`）。

---

## 相关文档

* 每个 demo 目录下的 `README.md` —— 展示内容、运行命令、退出码、关键源码位置
* [`../server/README.md`](../server/README.md) —— 预编译服务端
* [`../python/README.md`](../python/README.md) —— Python 接入面（ctypes + Cython KCP）
