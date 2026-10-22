# Harmonia Ork 压力测试系统

500 客户端 × 24 小时高压测试系统，用于验证 Ork 传输框架在高并发、长时间运行下的稳定性。

## 架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           192.168.31.225 (Mac)                              │
│  ┌─────────────────┐    ┌──────────────────┐    ┌────────────────────────┐ │
│  │     embed_minimal│◄───│  monitor_server  │◄───│  generate_stress_report│ │
│  │   (端口 37564)   │    │   (端口 39000)    │    │       (测试后执行)      │ │
│  └────────┬────────┘    └──────────────────┘    └────────────────────────┘ │
│           │                                                                 │
└───────────┼─────────────────────────────────────────────────────────────────┘
            │ TCP + KCP/UDP
            │
    ┌───────┴───────┐
    │               │
┌───▼───────────┐ ┌─▼─────────────┐
│ 192.168.31.140│ │ 192.168.31.96 │
│   (Linux)     │ │   (Linux)     │
│               │ │               │
│  5 workers    │ │  5 workers    │
│  × 50 clients │ │  × 50 clients │
│  = 250 客户端  │ │  = 250 客户端  │
│               │ │               │
│  node: lnx140 │ │  node: lnx96  │
└───────────────┘ └───────────────┘
```

**总计：500 客户端，24 小时持续负载**

## 快速开始

### 步骤 1：确认压测配置

仓库只提交脱敏模板。首次使用先执行 `cp stress/stress_config.example.yaml stress/stress_config.yaml` 并填写真实地址。压力测试需要以下覆盖值（定义在本地 `stress_config.yaml` 的 `server.config_overrides` 字段）：

| 配置项 | 覆盖值 | 说明 |
|--------|--------|------|
| `room.max_players` | 600 | 支持 500 客户端 + 余量 |
| `network.bootstrap_ttl_sec` | 172800 | 48h 加密会话有效期 |
| `log.level` | `Warn` | 减少日志 I/O 开销 |
| `network.bootstrap_security` | `plaintext_dev` | 压测环境无 TLS 证书时使用明文开发模式；**仅限单机压测**（loopback 硬绑定），跨机压测须 `tls` + 证书，见下节 |
| `network.per_ip_new_conn_burst` | 600 | 500 客户端集中建连，覆盖默认 per-IP burst=20 |
| `network.per_ip_new_conn_rate_per_min` | 3000 | 覆盖默认 60/min，避免建连被接纳门禁静默拒绝 |

这些值由 `package_server/run.sh` 自动读取，并在运行时复制到 `package_server/.run/config/` 覆盖层中注入，**无需手动修改仓库默认配置**。

检查命令：
```bash
grep -A6 config_overrides stress/stress_config.yaml
```

### Bootstrap Security 对压测的影响（feat/bootstrap-security）

服务端 `network.bootstrap_security` 自该分支起支持两种模式，对压测环境的影响如下：

- **`tls`（生产形态，跨机压测必选）**：TCP bootstrap 走 TLS 1.3，每次建连额外增加握手开销（本机参考约数十 ms，跨网络随 RTT 增大），计入 join_ms / 建连耗时指标；500 客户端各只建连一次，不影响稳态吞吐。需配置 `network.tls_certificate_file` / `network.tls_private_key_file`，证书缺失时服务端 fail closed 拒绝启动。
- **`plaintext_dev`（明文开发模式，仅限单机）**：无 TLS 握手开销、无需证书，但**硬绑定 loopback**——服务端只监听 `127.0.0.1`，客户端也拒绝连接非 loopback 目标。仅适用于 P0 单机快速验证（server + client 同机），且需以 `-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON` 构建（默认 OFF 构建下该配置被 Validate 拒绝，fail closed）。

> **跨机压测注意**：当前 Python 压测客户端（`OrkClientSession` / `worker.py`）尚未接入 TLS bootstrap，`tls` 模式下无法完成建连；跨机场景（140/96 → 225）需等待 Python 客户端 TLS 支持落地，或改用支持 TLS 的客户端。在此之前请先以 P0 单机模式验证。

> **建连限流（A0.2）**：资源保护默认 per-IP 限流（burst=20 / 60 每分钟）会把两个 worker 机器各 250 个客户端的集中建连大量静默拒绝（表现为 Join 大量超时）。压测必须按上表覆盖 `network.per_ip_new_conn_burst` / `network.per_ip_new_conn_rate_per_min`。

### 步骤 2：准备服务端分发包（embed_minimal）

在 **225 (Mac)** 上确认 ork-sdk-demo 的服务端分发包已就位（由 harmonia_ork 源仓打包）：

```bash
cd /path/to/ork-sdk-demo
ls -la server/macos-arm64/bin/embed_minimal
```

缺失时在 harmonia_ork 源仓执行 `bash scripts/pack_server_macos.sh`，将产物同步到 `server/macos-arm64/`（见 server/README.md）。

### 步骤 3：准备客户端机器环境

在 **140** 和 **96** 两台 Linux 机器上分别执行：

```bash
cd /path/to/ork-sdk-demo/python

# 创建虚拟环境（禁止复制 .venv，必须在目标机器重建）
python3 -m venv .venv
source .venv/bin/activate

# 安装依赖
pip install -r requirements.txt

# 编译 KCP bridge
bash lib/kcp/build.sh

# 验证
python -c "from client.ork_client_session import OrkClientSession; print('OK')"
```

### 步骤 4：复制部署包到目标机器

假设整个仓库已通过 rsync/scp 复制到各机器：

| 机器 | 部署包路径 | 说明 |
|------|-----------|------|
| 225 (Mac) | `stress/package_server/` | 服务端 + 监控 |
| 140 (Linux) | `stress/package_client_140/` | 250 客户端 |
| 96 (Linux) | `stress/package_client_96/` | 250 客户端 |

### 步骤 5：启动服务端（225）

```bash
cd stress/package_server
./run.sh
```

等待看到监控仪表盘显示 "服务端就绪"。

### 步骤 6：启动客户端（140 和 96）

在 **140** 上：
```bash
cd stress/package_client_140
./run.sh
```

在 **96** 上：
```bash
cd stress/package_client_96
./run.sh
```

## 日志监控

### 服务端 (225)

```bash
# 服务端日志
tail -f logs/ork.log

# 监控仪表盘自动显示在 run.sh 前台
```

### 客户端 (140/96)

```bash
# Worker 日志
tail -f stress/results/*/worker_*.log

# 实时指标（JSONL）
tail -f stress/results/*/*.jsonl

# 进程状态
ps aux | grep worker.py
```

## 渐进验证

建议按以下顺序逐步验证：

| 阶段 | 描述 | 命令 | 状态 |
|------|------|------|------|
| **P0** | 本机快速验证 | `./run.sh --quick`（在 225 上同时运行 server + client） | 可执行（单机，使用 `plaintext_dev`） |
| **P1** | 跨机快速验证 | 225: `./run.sh --quick`<br>140: `./run.sh --quick` | **阻塞**：等待 Python TLS 或改用原生 TLS 客户端 |
| **P2** | 单客户端节点满载（250 并发） | 225: `./run.sh`<br>140: `./run.sh`（250 客户端） | **阻塞**：等待 Python TLS 或改用原生 TLS 客户端 |
| **P3** | 双机满载 2h | 225: `./run.sh --duration-hours 2`<br>140+96: `./run.sh --duration-hours 2` | **阻塞**：等待 Python TLS 或改用原生 TLS 客户端 |
| **P4** | 正式 24h | 225: `./run.sh`<br>140+96: `./run.sh` | **阻塞**：等待 Python TLS 或改用原生 TLS 客户端 |

P1–P4 均为跨机场景（140/96 → 225）：跨机必须使用 `tls` 模式，但 Python 压测客户端尚未接入 TLS bootstrap，直接执行只会看到大量建连超时（详见上方「Bootstrap Security 对压测的影响」）。解除阻塞需等待 Python TLS 实现落地，或改用原生 TLS 客户端；在此之前仅 P0 可执行。

## 报告生成

测试结束后，在 **225** 上执行：

```bash
cd /path/to/ork-sdk-demo

# 找到最新的 run_id
ls -lt stress/results/ | head

# 生成报告
python stress/generate_stress_report.py \
  --results stress/results/<run_id> \
  --output stress/reports/<run_id>.md \
  --config stress/stress_config.yaml
```

**注意**：需要先将 140 和 96 的结果复制到 225：

```bash
# 在 140 上
cd stress/results
tar czf lnx140_results.tar.gz <run_id>/
scp lnx140_results.tar.gz user@192.168.31.225:~/ork-sdk-demo/stress/results/

# 在 96 上
cd stress/results
tar czf lnx96_results.tar.gz <run_id>/
scp lnx96_results.tar.gz user@192.168.31.225:~/ork-sdk-demo/stress/results/

# 在 225 上解压
cd stress/results
tar xzf lnx140_results.tar.gz
tar xzf lnx96_results.tar.gz
```

## 配置说明

`stress_config.yaml` 字段说明：

```yaml
run:
  id: "stress_500_24h_v1"      # 运行 ID，用于结果目录命名
  duration_hours: 24            # 测试持续时间（小时）
  stats_interval_sec: 60        # 统计上报间隔（秒）

server:
  host: 192.168.31.225          # 服务端 IP
  port: 37564                   # 服务端端口
  config_overrides:             # 由 package_server/run.sh 自动读取并注入运行时配置副本
    room.max_players: 600
    network.bootstrap_ttl_sec: 172800
    log.level: Warn
    network.bootstrap_security: plaintext_dev   # 单机压测无证书时使用；跨机须 tls + 证书
    network.per_ip_new_conn_burst: 600          # 覆盖默认 20，避免建连被限流拒绝
    network.per_ip_new_conn_rate_per_min: 3000  # 覆盖默认 60/min

nodes:                          # 客户端节点列表
  - name: lnx140                # 节点名称（需与 run.sh --node 匹配）
    host: 192.168.31.140        # 节点 IP
    ssh_user: root              # SSH 用户（coordinator.sh 使用）
    workers: 5                  # Worker 进程数
    clients_per_worker: 50      # 每 Worker 客户端数
    work_dir: ~/ork-sdk-demo    # 工作目录
  - name: lnx96
    host: 192.168.31.96
    ssh_user: root
    workers: 5
    clients_per_worker: 50
    work_dir: ~/ork-sdk-demo

workload:                       # 负载配置
  pose_hz: 50                   # Pose 发送频率（Hz）
  reliable_hz: 5                # Reliable 消息频率（Hz）
  batch_hz: 10                  # Entity Batch 频率（Hz）
  ack_hz: 10                    # Ack 频率（Hz）
  bone_count: 17                # 骨骼数量（Pose 大小）
  batch_entities: 12            # 每 Batch 实体数
  connect_stagger_ms: 100       # 连接错开间隔（ms）
  join_timeout_sec: 30          # Join 超时（秒）

monitoring:
  port: 39000                   # 监控服务端口
  dashboard_refresh_sec: 5      # 仪表盘刷新间隔（秒）
```

## 常见问题

### 1. Join 被拒绝 / 客户端连接失败

**原因**：服务端并发连接数超过 `max_players` 限制。

**解决**：确认 `stress_config.yaml` 中 `config_overrides` 包含 `room.max_players: 600`（默认已配置，`package_server/run.sh` 自动注入）。

### 2. 5 分钟后加密失败

**原因**：`stress_config.yaml` 中 `config_overrides` 未设置 `network.bootstrap_ttl_sec: 172800`。

**解决**：
```bash
# 编辑 stress_config.yaml，确保 config_overrides 包含：
#   network.bootstrap_ttl_sec: 172800
```

### 3. ImportError: KCP bridge

**原因**：`libkcp_bridge.so` 未编译。

**解决**：
```bash
cd python/lib/kcp
bash build.sh
```

### 4. Too many open files

**原因**：系统 fd 上限太低。

**解决**：
```bash
ulimit -n 65535

# 永久生效（Linux）
echo "* soft nofile 65535" >> /etc/security/limits.conf
echo "* hard nofile 65535" >> /etc/security/limits.conf
```

### 5. ModuleNotFoundError: generated

**原因**：Proto 生成文件缺失。

**解决**：
```bash
ls python/generated/pb_compat.py
```

generated/ 已随仓库分发；若缺失请从开发机同步 `python/generated/` 目录。

### 6. 客户端无法连接服务端

**检查清单**：
1. 服务端已启动：`lsof -i :37564`（在 225 上）
2. 网络连通：`ping 192.168.31.225`
3. 端口可达：`nc -zv 192.168.31.225 37564`
4. 防火墙规则允许 TCP/UDP 37564 和 TCP 39000

### 7. Join 大量超时 / 建连被拒（Bootstrap Security 相关）

**检查清单**：
1. `network.bootstrap_security` 模式：`plaintext_dev` 仅限单机（loopback 硬绑定）；跨机必须 `tls`（当前 Python 客户端尚未接入 TLS bootstrap，见「Bootstrap Security 对压测的影响」）
2. per-IP 限流覆盖：`config_overrides` 须含 `network.per_ip_new_conn_burst: 600` 与 `network.per_ip_new_conn_rate_per_min: 3000`
3. `tls` 模式下证书路径有效（`tls_certificate_file` / `tls_private_key_file`），缺失则服务端拒绝启动

## 自动编排（可选）

如果已配置 SSH 免密登录，可使用 `coordinator.sh` 一键启动所有节点：

```bash
cd demo/stress

# 快速验证
./coordinator.sh --quick

# 正式测试
./coordinator.sh

# 只打印命令，不执行
./coordinator.sh --dry-run
```

**前提**：
- 225 可 SSH 免密登录到 140 和 96
- 各机器仓库路径与 `stress_config.yaml` 中 `work_dir` 一致

## 目录结构

```
stress/
├── README.md                    # 本文件
├── stress_config.yaml           # 全局配置
├── coordinator.sh               # SSH 自动编排（可选）
├── deploy.sh                    # 远程部署脚本（可选）
├── worker.py                    # 客户端 Worker
├── monitor_server.py            # 监控聚合服务
├── generate_stress_report.py    # 报告生成器
├── lib/
│   └── metrics.py               # 指标库
├── package_server/              # 服务端部署包 (225)
│   ├── run.sh
│   └── README.md
├── package_client_140/          # 客户端部署包 (140)
│   ├── run.sh
│   └── README.md
├── package_client_96/           # 客户端部署包 (96)
│   ├── run.sh
│   └── README.md
├── results/                     # 测试结果
│   └── <run_id>/
│       ├── timeseries.jsonl
│       ├── server_stats.jsonl
│       ├── worker_*.log
│       └── *.jsonl
└── reports/                     # 生成的报告
    └── <run_id>.md
```

## 相关文档

- [服务端部署包说明](package_server/README.md)
- [客户端部署包说明 (140)](package_client_140/README.md)
- [客户端部署包说明 (96)](package_client_96/README.md)
- [Benchmark 测试](../benchmark/README.md)
- [Validation 测试](../validation/README.md)
