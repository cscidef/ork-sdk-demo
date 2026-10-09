# Demo 02 · 最小回显客户端（纯 C11 / C ABI Client）

用 **一个 `.c` 文件、只 include `ork_client_abi.h`** 走完 Ork 客户端的完整链路，
并展示 C ABI Client 与 Host 最大的差别：**客户端没有内部线程，必须自己
`ork_client_pump_once()` 驱动**。

## 展示内容

| 阶段 | 说明 |
|------|------|
| TCP Bootstrap | 连上后接收 **Bootstrap V2（109B opaque 帧）**：connectionId / sessionToken / c2sKey / s2cKey / bootstrapEpoch / 加密标志 |
| UDP + KCP | 绑定 UDP socket，创建 KCP 会话（`conv = connectionId`） |
| Handshake V2 | 发送 42B 认证握手帧（HKDF 派生 handshakeKey + HMAC-SHA256 authTag） |
| 等待 ACK | 收到服务端经 KCP 下发的 `"ORK-UDP-ACK:handshake"` → 连接就绪 |
| 双通道 Echo | Reliable(KCP) 与 Unreliable(UDP) 各发 25B `ORKBENCH1` 帧，逐字节校验回显 |

回显帧格式：`"ORKBENCH1"(9B) + marker(8B) + 发送时间戳 ns(8B 小端) = 25B`
（服务端 `TransportDemoHandler` 的唯一 opaque 回显路径；长度不是 25 或前缀不对
会被当作 protobuf 尝试解析）。

## C ABI Client 全部 9 个函数

```c
OrkError        ork_client_create(const OrkClientConfig*, const OrkClientCallbacks*, OrkClientHandle*);
void            ork_client_destroy(OrkClientHandle);
OrkError        ork_client_connect(OrkClientHandle);
OrkError        ork_client_disconnect(OrkClientHandle);      // 幂等
OrkError        ork_client_pump_once(OrkClientHandle);        // ★ 真正的驱动：io.poll() + io.restart()
OrkSendResult   ork_client_send(OrkClientHandle, const uint8_t*, uint32_t, OrkChannel);
OrkClientState  ork_client_get_state(OrkClientHandle);
uint32_t        ork_client_get_connection_id(OrkClientHandle);
const char*     ork_client_last_error(void);
```

## `OrkClientConfig` 的两个版本

| 版本 | `struct_size` | 差异 |
|------|---------------|------|
| **v1** | `ORK_CLIENT_CONFIG_V1_SIZE` = **72** | 无 bootstrap 安全字段；语义回落到默认（TLS），不读尾部字段 |
| **v2** | `sizeof(OrkClientConfig)` = **96**（LP64） | 尾部多 4 个 TLS / bootstrap 字段：`bootstrap_security_mode`、`tls_ca_file`、`tls_expected_server_name`、`tls_use_system_trust_store`、`tls_handshake_timeout_ms` |

`create` 对 `struct_size` 做的是 **精确版本白名单**（只接受 72 与 96）——
居中值、更大值一律 `OrkErr_InvalidArg`，杜绝部分填充与未来版本被静默截断。
本 demo 用 v2，并显式设 `bootstrap_security_mode = OrkBootstrapSecurity_PlaintextDev`
以匹配 `ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON` 构建的服务端。

## 状态机

```
Disconnected → Connecting → Bootstrapping → Handshaking → Connected
     ↑                                                          │
     └──────────────────── Disconnected ◄───────────────────────┘
```

`on_connected` 触发时状态**已经是** `Connected`，KCP 会话也已建好，所以
**在 `on_connected` 回调里直接 `ork_client_send()` 是安全的**。

## 线程契约

客户端所有回调都在 **调用 `ork_client_pump_once()` / `ork_client_disconnect()`
的那个线程的调用栈内** 触发（本 demo 是主线程），因此：

* 回调里的普通变量不需要 atomic
* ❌ 回调里 **不能** 调 `ork_client_destroy()`（正在析构的对象会在回调返回后继续被使用）
* 需要销毁时置个标志，回到主循环外再做

## 运行

```bash
bash ../server/start.sh                             # 或另开终端跑 01_echo_server
./build/bin/02_echo_client                          # 连 127.0.0.1:9400
./build/bin/02_echo_client --port 9500 --count 3
./build/bin/02_echo_client --timeout 20 --host 127.0.0.1
```

客户端加密意向恒为开（`cfg.enable_encryption = 1`）；**权威开关在服务端**
（`BootstrapFrame.flags`），所以实际是否加密由对端决定。要看两轮对照请去
[`../06_encryption`](../06_encryption/)。

> **输出噪音**：`OrkClientConfig` **没有** `log_path` 字段，客户端侧的 SDK 内部
> 日志在 Logger 未 Init 时全部走 **stderr**（形如 `[ork][pre-init] …`，还会带上游
> 源码路径）。想要纯净的 demo 输出：
> ```bash
> ./build/bin/02_echo_client 2>/dev/null
> ```

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 连接建立 + 双通道 echo 全部逐字节一致 |
| `1` | 任一失败（连接超时 / 回显不一致 / send 返回非 Ok） |
| `2` | 命令行参数非法 |

## 源码

[`main.c`](main.c) · [`CMakeLists.txt`](CMakeLists.txt)
