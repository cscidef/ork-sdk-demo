# Demo 01 · 最小回显服务端（纯 C11 / C ABI Host）

用 **一个 `.c` 文件、只 include 一个头** 起一个完整的 Ork 服务端，展示 C ABI Host
全部 13 个导出函数的正确调用顺序与线程契约。

## 展示内容

| 步骤 | C ABI | 说明 |
|------|-------|------|
| 1 | `ork_abi_version()` | 运行期 ABI 版本必须等于编译期 `ORK_ABI_VERSION`，否则结构体布局会漂移 |
| 2 | `ork_host_create(&cfg, &cb, &host)` | 填 `OrkHostConfig`（`struct_size` 必填）+ `OrkHostCallbacks`（四个函数指针 + `void* user`） |
| 3 | `on_connected` | 新连接接入。回调里调 `ork_host_get_connection_info()` 打印 link / encryption_active |
| 4 | `on_payload` | **原样、同通道回显**。`data` 的生命周期仅限本次回调，要留存必须自己拷贝 |
| 5 | `on_disconnected` | 打印 `OrkDisconnectReason` |
| 6 | `on_stats` | 约 1Hz，缓存 `OrkStats` 标量供主线程打印心跳 |
| 7 | `ork_host_start(host)` | 启动内部 pump 线程，同时监听 TCP bootstrap 与 UDP/KCP（**同一端口**） |
| 8 | 主循环 | 打印心跳；`ork_host_pump_once()` 只是 1ms 协作让出，不驱动网络 |
| 9 | `SIGINT` / `SIGTERM` | 只置原子标志，清理回主线程做（`ork_host_stop()` 会 join pump 线程） |
| 10 | `ork_host_stop()` → `ork_host_destroy()` | 干净收尾；`destroy(NULL)` 是安全的 no-op |

## 线程契约（关键）

四个回调都在 **core 内部 pump 线程** 上触发：

* ✅ 回调里可以调 `ork_host_send` / `ork_host_broadcast` / `ork_host_disconnect` /
  `ork_host_get_stats` / `ork_host_get_connection_info`（只入队 / 只读快照）
* ❌ 回调里 **绝不能** 调 `ork_host_stop()` 或 `ork_host_destroy()` —— 它们要 join
  当前所在线程 → **自死锁**
* 主线程读回调写的计数器一律用 `atomic`

## 运行

```bash
cd cpp && cmake -B build && cmake --build build -j

# 终端 1
./build/bin/01_echo_server                      # 监听 9400
./build/bin/01_echo_server 9500                 # ★ 端口是位置参数，不是 --port
./build/bin/01_echo_server --help

# 终端 2
./build/bin/02_echo_client --port 9500
```

本 demo 刻意把参数面缩到最小（只有一个可选端口），AEAD 加密恒为开
（`cfg.enable_encryption = 1`）—— 要看关加密的对照组请去
[`../06_encryption`](../06_encryption/)。

日志走 C ABI 自己的 `log_path` / `log_level` 字段，默认 `/dev/null` + `warn`；
环境变量 `ORK_LOG_FILE` / `ORK_LOG_LEVEL` 可覆盖（`ORK_LOG_LEVEL=off` 连明文
bootstrap 的 critical 告警一起抑制）。

## 退出码

| 码 | 含义 |
|----|------|
| `0` | 收到 SIGINT / SIGTERM 后干净退出 |
| `1` | `create` / `start` 失败（`ork_last_error()` 会打印原因） |
| `2` | 命令行参数非法 |

## 排障

* **`ork_host_start` 失败：地址已被占用** —— 9400 上可能已经有 `server/start.sh`
  起的 `embed_minimal`；换端口或先 `bash ../server/stop.sh`。
* **`create` 报 `kcp_update_rate_hz must be >= pump_rate_hz`** —— 这两个字段的
  约束是 `kcpUpdateRate >= pumpRate`，别把它们写反。
* **客户端连上了但收不到回显** —— 检查客户端的 `udp_port` 是否也设成了同一个值
  （留 0 会回落 9401）。

## 源码

[`main.c`](main.c) · [`CMakeLists.txt`](CMakeLists.txt)
