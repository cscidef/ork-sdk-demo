# Harmonia Ork 对比基准

本目录提供可重复执行的协议对比测试，并生成 Markdown 报告。它比较的是同一负载下的传输行为，不把最小 echo 基线包装成“完整传统游戏服务器”。

## 快速执行

```bash
cd benchmark
./run_benchmark.sh --quick
```

报告输出到 `reports/latest.md`，原始 JSON 输出到 `results/<run-id>/`。

完整测试：

```bash
./run_benchmark.sh
```

一次生成 1/10/20 客户端的统一容量报告：

```bash
./run_benchmark.sh --suite --duration 30 --repetitions 5
```

只重新生成报告：

```bash
python3 generate_report.py --results results/<run-id> --output reports/report.md
```

## 被测方案

- `tcp_only`：长度前缀 TCP echo，所有流量可靠传输。
- `udp_only`：UDP echo，所有流量均允许丢失。
- `tcp_udp`：可靠消息走 TCP，实时消息走 UDP。
- `ork_hybrid`：真实 `ork_room_server`，TCP bootstrap + RUDP/KCP + UDP + OrkCipher。

前三组由同一个 C++ reference server 提供，使用同一个二进制和统一 wire payload。Ork 使用仓库真实 SDK demo。由于 Ork 带有 Protobuf、会话、加密和 Tick，而基线是最小 echo，报告会明确标记以下限制：

- 可靠 RTT 可以比较，但 Ork 数值包含完整会话和协议成本。
- offered load 和 Join 成功率可以比较；CPU/内存尚未自动采集，正式容量报告需要配合系统监控。
- Ork Hybrid 使用 ORKBENCH1 opaque ping 分别实测 Reliable 和 Unreliable RTT/送达率。
- reference server 不是生产级游戏服，只代表传统传输下界。

## 网络环境

默认不修改系统网络。Linux 可手动使用 `tc netem`，然后把环境名称写入报告：

```bash
sudo ./netem.sh apply eth0 mobile
BENCH_NETWORK=mobile ./run_benchmark.sh
sudo ./netem.sh clear eth0
```

macOS 不内置自动网络整形，建议使用 Network Link Conditioner，或在独立 Linux 网关执行 `netem.sh`。

## 常用参数

```bash
./run_benchmark.sh \
  --clients 20 \
  --duration 30 \
  --repetitions 5 \
  --pose-hz 30 \
  --reliable-hz 5 \
  --payload-bytes 256 \
  --network mobile
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--clients N` | 10 | 并发客户端数 |
| `--duration SEC` | 15 | 每轮测试持续秒数 |
| `--repetitions N` | 3 | 重复次数（取中位数） |
| `--pose-hz HZ` | 30 | UDP 位姿发送频率 |
| `--reliable-hz HZ` | 5 | RUDP 可靠消息频率 |
| `--payload-bytes N` | 256 | 消息体大小 |
| `--network PROFILE` | unshaped | 网络环境档位：`unshaped`/`broadband`/`mobile`/`weak` |
| `--quick` | — | 快速档（少客户端、短时长） |
| `--suite` | — | 生成 1/10/20 客户端统一容量报告 |

`--network` 启用应用层注入（impairment.py），无需 sudo，可在 CI 中使用。Linux 下还可配合 `netem.sh` 做内核级网络整形。

测试机应使用 Release 构建、关闭调试日志，并在独立客户端机器上复跑正式数据。本地 `--quick` 主要用于验证脚本和建立趋势，不适合作为最终容量结论。
