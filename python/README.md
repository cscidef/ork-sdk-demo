# Ork SDK · Python Demo

Ork 纯传输 SDK 的 Python 调用示例套件：从最小回显到多客户端并发、加密通道、
时延测量，全部走 **真实传输路径**（TCP bootstrap + UDP/KCP 双通道），无模拟桩。

> Ork 的传输层：客户端先经 TCP 拿到 **Bootstrap V2（109B）** 会话凭证，再在
> **同一端口** 上用 UDP 建立 KCP 可靠通道与裸 UDP 不可靠通道，经 **Handshake V2
> （42B）** 认证后进入 Connected，双向收发 opaque 载荷，可选 ChaCha20-Poly1305
> AEAD 加密。

---

## 目录结构

```
python/
├── README.md                 # 本文件
├── requirements.txt          # 依赖（PyNaCl + pytest）
├── setup.sh                  # 一键：venv → 依赖 → 编译 KCP → 离线自检
├── lib/                      # 公共库
│   ├── ork_frame.py          # Bootstrap V2 / Handshake V2 / 15B 帧头编解码（纯 stdlib）
│   ├── ork_cipher.py         # ChaCha20-Poly1305-IETF AEAD 传输密码层（PyNaCl）
│   ├── ork_client_session.py # 单客户端完整会话（bootstrap→handshake→双通道收发）
│   ├── demo_helpers.py       # demo 共享：连接编排 / echo / RTT 统计 / CLI
│   └── kcp/                  # KCP 可靠传输 C bridge + ctypes 绑定 + build.sh
├── demos/                    # 6 个可独立运行的示例（各带 README）
│   ├── 01_echo/              # 最小回显（双通道）
│   ├── 02_reliable_vs_unreliable/  # KCP vs UDP 送达率 + 时延对比
│   ├── 03_encryption/        # 密码层帧结构剖析 + 加密 echo
│   ├── 04_connection_lifecycle/    # 连接→发送→空闲→断开→重连
│   ├── 05_multi_client/      # N 客户端并发，聚合成功率
│   └── 06_latency_measure/   # 双通道 RTT min/avg/p50/p95/p99/max
├── tests/                    # pytest（离线 + 在线，自动启停服务端）
└── testdata/                 # payloads（各尺寸）+ golden（冻结 hex 向量）
```

---

## 前置条件

- **Python 3.10+**（库使用 `X | Y` 类型语法）
- **C 编译器**（clang / gcc）—— 编译 KCP bridge 共享库
- **PyNaCl**（libsodium 绑定，提供 ChaCha20-Poly1305）—— 由 setup.sh 安装
- demo 服务端（仓库根 `server/`，已预编译，macOS arm64）

---

## 快速开始

```bash
cd python

# 1) 安装环境：创建 .venv、装依赖、编译 KCP bridge、跑离线自检
bash setup.sh
source .venv/bin/activate

# 2) 启动 demo 服务端（默认端口 9400，TCP 与 UDP/KCP 共用）
bash ../server/start.sh

# 3) 运行任意 demo
python demos/01_echo/client.py
python demos/06_latency_measure/client.py --pings 200

# 4) 运行测试（conftest 会自动启停服务端；已在运行则复用）
pytest tests/ -v

# 5) 结束：关停服务端
bash ../server/stop.sh
```

> 服务端与客户端 **共用同一端口**（默认 9400）：TCP bootstrap 与 UDP/KCP 都走
> 9400，没有独立的 UDP 端口。可用 `GAME_PORT` 环境变量改服务端端口，客户端用
> `--port` 指定。

---

## Demo 一览

| # | 名称 | 展示 | 关键参数 |
|---|------|------|----------|
| 01 | echo | 最小链路 + 双通道 25B `ORKBENCH1` 回显 | `--count` |
| 02 | reliable_vs_unreliable | KCP 与 UDP 送达率 + RTT 分位对比 | `--iterations` |
| 03 | encryption | 帧结构剖析、seal/open、重放/篡改拒绝、加密 echo | `--rounds` `--offline-only` |
| 04 | connection_lifecycle | 状态机：connect→send→idle→disconnect→reconnect | `--idle` `--reconnects` |
| 05 | multi_client | N 客户端并发 echo，聚合成功率 | `--clients` `--echoes` |
| 06 | latency_measure | 双通道 RTT min/avg/p50/p95/p99/max | `--pings` `--warmup` |

每个 demo 都支持 `--host`（默认 127.0.0.1）与 `--port`（默认 9400），退出码
`0`=成功、`1`=失败，详见各自目录下的 `README.md`。

---

## 回显约束（重要）

demo 服务端的 `TransportDemoHandler` 只对 **精确 25 字节、前缀 `ORKBENCH1`** 的
opaque 帧按原通道回显；其余字节按 protobuf 解析，失败即丢弃。因此所有 echo 类
demo/测试都通过 `send_bench_*` / `drain_bench_*` 走这条唯一回显路径：

```
ORKBENCH1 帧 = b"ORKBENCH1"(9B) + seq(8B LE) + send_time_ns(8B LE) = 25B
```

---

## 测试

```bash
pytest tests/ -v                 # 全部（离线 + 在线）
pytest tests/test_frame_codec.py tests/test_cipher.py -v   # 仅离线（无需服务端）
```

- **离线**：`test_frame_codec.py`（Bootstrap V2 / Handshake V2 / 15B 帧头编解码 +
  golden 交叉校验）、`test_cipher.py`（AEAD seal/open、重放、篡改、payload 往返）
- **在线**：`test_echo.py`、`test_channels.py`、`test_encryption.py`、
  `test_lifecycle.py`、`test_multi_client.py`
- `tests/conftest.py` 的 session 级 `server` fixture 自动启停服务端：端口已被监听
  则复用（不关停），否则 `start.sh` 启动、等待端口就绪、结束后 `stop.sh` 关停。
  用 `ORK_TEST_PORT` 指定测试端口。

---

## testdata

```bash
python testdata/generate.py      # 重新生成 payloads/ 与 golden/
```

- `payloads/`：1 / 64 / 512 / 1200 字节，第 i 字节 = `(i*31+7)&0xFF` 的可预测模式
- `golden/`：Bootstrap V2、Handshake V2(counter 0/1)、cipher seal 的冻结 hex，与
  C++ 参考实现逐字节对齐；输入参数见 `golden/INPUTS.md`

---

## 安全说明

demo 服务端配置为 `bootstrap_security: plaintext_dev`（loopback 演示用途）：
bootstrap 交换本身不套 TLS，但 **数据通道仍启用 ChaCha20-Poly1305 AEAD 加密**
（Bootstrap flags 置 encryption_required）。生产部署请在 bootstrap 层启用 TLS 并
使用正式密钥管理。
