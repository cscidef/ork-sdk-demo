# 压力测试服务端部署包 (Mac 225)

本目录包含在 **192.168.31.225 (Mac)** 上运行 Harmonia Ork 压力测试服务端的所有脚本。

## 前置条件

### 1. 服务端分发包（embed_minimal）

```bash
cd /path/to/ork-sdk-demo
ls -la server/macos-arm64/bin/embed_minimal
```

缺失时在 harmonia_ork 源仓执行 `bash scripts/pack_server_macos.sh`，将产物同步到 `server/macos-arm64/`（见 server/README.md）。

### 2. 调整配置文件

压力测试需要以下配置（`run.sh` 会自动检查并提示）：

| 文件 | 配置项 | 要求值 | 说明 |
|------|--------|--------|------|
| `server/config/room.yaml` | `max_players` | ≥ 600 | 支持 500 客户端 + 余量 |
| `server/config/network.yaml` | `bootstrap_ttl_sec` | ≥ 172800 | 48h 加密会话有效期 |
| `server/config/log.yaml` | `level` | `Warn` | 减少日志 I/O 开销 |

手动修改命令：
```bash
# macOS sed 需要 '' 参数
sed -i '' 's/max_players:.*/max_players: 600/' server/config/room.yaml
sed -i '' 's/bootstrap_ttl_sec:.*/bootstrap_ttl_sec: 172800/' server/config/network.yaml
sed -i '' 's/level:.*/level: Warn/' server/config/log.yaml
```

### 3. Python 环境

`monitor_server.py` 需要 Python 3.9+ 和 PyYAML：

```bash
cd python
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## 执行步骤

### 快速验证模式（5 分钟）

```bash
cd stress/package_server
./run.sh --quick
```

### 正式 24 小时测试

```bash
cd stress/package_server
./run.sh
```

### 自定义时长

```bash
cd stress/package_server
./run.sh --duration-hours 8
```

## 运行后

脚本启动后会：
1. 后台运行 `embed_minimal`（日志写入 `server/logs/ork.log`）
2. 前台显示监控仪表盘（`monitor_server.py`）
3. 等待客户端连接

### 查看服务端日志

```bash
tail -f logs/ork.log
```

### 查看实时指标

监控仪表盘自动显示：
- 客户端连接数 / Join 成功率
- 消息吞吐（Pose/Reliable/Batch/Ack）
- RTT 延迟（P50/P95/P99）
- 服务端 CPU / 内存 / 网络带宽

### 停止服务

按 `Ctrl+C` 停止所有服务（`run.sh` 会自动清理 `embed_minimal` 和 `monitor_server`）。

## 测试结束后生成报告

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

## 目录结构

```
stress/
├── package_server/
│   ├── run.sh          # 本脚本
│   └── README.md       # 本文件
├── results/
│   └── <run_id>/
│       ├── timeseries.jsonl      # Worker 上报数据
│       └── server_stats.jsonl    # 服务端指标
├── reports/
│   └── <run_id>.md               # 生成的报告
├── stress_config.yaml            # 全局配置
├── monitor_server.py             # 监控服务
├── worker.py                     # 客户端 Worker
└── generate_stress_report.py     # 报告生成器
```

## 故障排查

### ork server 启动失败

```bash
# 查看启动日志
cat server/logs/ork_server_stdout.log

# 检查端口占用
lsof -i :37564
```

### 客户端无法连接

1. 确认防火墙允许 37564 (TCP/UDP) 和 39000 (TCP) 端口
2. 确认 `max_players` 已调整
3. 查看服务端日志：`tail -f logs/ork.log`

### monitor_server 报错

```bash
# 手动测试 Python 环境
source python/.venv/bin/activate
python -c "import yaml; print('OK')"
```
