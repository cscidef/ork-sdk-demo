# Harmonia Ork 通讯方案对比测试报告

> 生成时间：2026-09-18T04:28:00+08:00  
> 原始数据：`/Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork/demo/benchmark/results/20260918_042553`  
> 测试主机：macOS-26.6.2-arm64-arm-64bit-Mach-O / Python 3.14.6

## 0. 执行摘要

- Ork 各并发档位的平均连接成功率：100.00%。
- Ork 可靠消息各组平均送达率：100.00%。
- Ork 记录到握手重试 0 次、客户端错误 0 条。
- 本机无损回环主要验证功能、稳定性和容量趋势；传输方案优劣必须结合弱网结果判断。
- reference server 是最小 echo 下界。若 Ork RTT 高于它，代表完整协议栈成本，不等同于游戏体验更差；若要证明混合通道价值，应关注丢包环境下可靠消息尾延迟和实时消息新鲜度。
- 本次基线的 Build Type、source revision、CPU/硬件标识、固定参数、复现命令、资源采样与外推边界见第 6 节。

## 1. 测试目的

在统一并发数、消息频率和测试时长下，对比 TCP-only、UDP-only、传统 TCP+UDP 与 Harmonia Ork Hybrid。报告关注连接成功率、可靠消息 RTT、消息送达率和业务负载吞吐。

## 2. 被测实现与公平性边界

| 方案 | 实现 | 说明 |
|---|---|---|
| TCP-only | C++ reference echo | 可靠与实时消息都经过 TCP |
| UDP-only | C++ reference echo | 所有消息经过裸 UDP |
| 传统 TCP+UDP | C++ reference echo | 可靠消息走 TCP，实时消息走 UDP |
| Ork Hybrid | 真实 ork_room_server | TCP bootstrap、RUDP/KCP、UDP、Protobuf、会话、Tick 与配置中的加密策略 |

> 重要：reference server 是传输下界，不是完整传统游戏服务器。Ork 承担更多协议与会话工作，因此本报告不会把单次本机结果泛化成"任何环境都更快"。Phase 4.0 起 Ork 使用 ORKBENCH1 opaque ping 同时测量 Reliable 和 Unreliable 通道的 RTT 与送达率。

## 3. 汇总结果

| 网络 | 客户端 | 方案 | 重复 | 连接成功率 | Join/连接 P95 ms | 可靠送达率 | 可靠 RTT P50/P95/P99 ms | 实时送达率 | 实时 RTT P95 ms | UDP RTT P50 ms | UDP RTT P95 ms | UDP RTT P99 ms | UDP 送达率 | Offered Mbps | 握手重试 | 错误 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| unshaped | 5 | Ork Hybrid | 3 | 100.00% | 31.48 | 100.00% | 9.27/13.32/13.92 | 100.00% | 18.74 | 10.08 | 18.74 | 20.38 | 100.00% | 0.081 | 0 | 0 |
| unshaped | 5 | TCP-only | 3 | 100.00% | 1.56 | 100.00% | 0.16/0.24/0.32 | 100.00% | 0.24 | — | — | — | — | 0.364 | 0 | 0 |
| unshaped | 5 | 传统 TCP+UDP | 3 | 100.00% | 1.33 | 100.00% | 0.18/0.26/0.38 | 100.00% | 0.23 | 0.13 | 0.23 | 0.28 | 100.00% | 0.359 | 0 | 0 |
| unshaped | 5 | UDP-only | 3 | 100.00% | 0.07 | 100.00% | 0.14/0.23/0.33 | 100.00% | 0.22 | 0.13 | 0.22 | 0.29 | 100.00% | 0.358 | 0 | 0 |

## 4. 结果判读

正式结论应以至少 5 次独立重复的均值和波动范围为依据，并重点观察 P95/P99，而不是只看平均延迟。建议按以下规则判读：

- 实时性：可靠 RTT P95/P99 越低越好；TCP 在丢包下可能出现队头阻塞。
- 可靠性：可靠送达率应接近 100%；UDP-only 的结果不代表业务可靠语义。
- 可用性：连接成功率和错误数用于识别容量拐点。
- 带宽：Offered Mbps 是客户端产生的业务负载，不等于网卡实际字节；正式测试应补充系统网络计数器。
- 负载大小：reference 的 `payload_bytes` 是固定 wire payload；Ork 使用真实 Protobuf 游戏消息，大小由字段和加密帧决定，因此 Offered Mbps 只反映各方案实际产生的负载，不应直接解释成协议开销率。
- Ork 实时通道：Phase 4.0 起使用 ORKBENCH1 opaque ping 测量 UDP 端到端 RTT，不再是 N/A；结果反映本机回环下完整协议栈（加密帧、UDP 发送/接收、OrkFrame 解包）的往返延迟。
- UDP 实测列（Phase 6.3）：`UDP RTT P50/P95/P99` 与 `UDP 送达率` 来自 Unreliable 通道 ORKBENCH1 往返，送达率=received/sent；原始 JSON 另含 `udp_loss_rate`（1-送达率）与 `udp_reorder_rate`（收到 seq 非单调递增的比例）。本机无损回环下 UDP 送达率应 ≥99%，且 `udp_rtt_p50` 应低于 `rudp_rtt_p50`（UDP 无重传/拥塞控制）。
- UDP 列显示 “—” 表示该方案无独立 UDP 通道（如 TCP-only，其实时消息也走 TCP），非数值 0。

## 5. 有效结论模板

请基于表格填写，不要预先写入‘Ork 全面优于所有方案’：

> 在【网络条件】、【客户端数】和【混合负载】下，Ork 的可靠消息 P95/P99 RTT 为【数值】，可靠送达率为【数值】。相较【基线】，改善/退化【数值】；同时 Ork 提供连接生命周期、可靠/不可靠分流、加密和 Tick 集成。其代价或未覆盖项为【CPU/带宽/UDP 回显限制】。

## 6. 可复现性与环境元数据

| 项目 | 值 |
|---|---|
| Build Type | Release |
| Source revision | `9353925c7090`（phase-6.2 应用层弱网损伤注入与 dnctl 脚本） |
| CPU | Apple M5，10 cores |
| 硬件 / 内存 | Mac17,2 / 32 GB |
| Python | 3.14.6 |
| 加密 | enabled（服务器日志 SessionFramework encryption=true） |
| 拓扑 | 同机回环 127.0.0.1（服务器与客户端同主机） |
| 数据采集时段 | 2026-09-18T04:28:00+08:00 |

> 以上字段来自 `results/<run_id>/run_metadata.json`（历史基线为按 CMakeCache / git / sysctl / 服务器日志核实后补录，可复核；后续运行由 `run_benchmark.sh` 自动采集）。缺失项如实标注，不以事后查询值补录。

### 6.1 固定参数

| 参数 | 值 |
|---|---|
| NETWORK | unshaped |
| CLIENTS | 5 |
| DURATION_S | 10.0 |
| REPETITIONS | 3 |
| POSE_HZ | 30.0 |
| RELIABLE_HZ | 5.0 |
| PAYLOAD_BYTES | 256 |
| BONE_COUNT | 8 |

> 参数来源：本次采集实际参数（原始 JSON）。

### 6.2 构建目录与可复现命令

```bash
# 构建（等效命令，参数与 CMakeCache 一致）
cmake -S . -B /Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork/build-release -DCMAKE_BUILD_TYPE=Release
cmake --build /Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork/build-release --target ork_room_server -j4

# 采集（等效复现命令，参数与原始数据一致）
cd demo/benchmark
ORK_BUILD_DIR=/Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork/build-release run_benchmark.sh --clients 5 --duration 10 --repetitions 3 --network unshaped
```

> 构建目录：`/Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork/build-release`（Build Type 以其 `CMakeCache.txt` 为准）。

### 6.3 系统资源采样（构建 / benchmark 前后）

| 时点 | Load avg (1m/5m/15m) | 内存 | Swap 使用 |
|---|---|---|---|
| 构建前 | 2.00 / 2.32 / 2.34 | 可用 8.39 GB / 共 32.00 GB | 3.35 GB |
| 构建后 | 1.92 / 2.30 / 2.33 | 可用 8.37 GB / 共 32.00 GB | 3.35 GB |
| benchmark 前 | 1.92 / 2.30 / 2.33 | 可用 8.37 GB / 共 32.00 GB | 3.35 GB |
| benchmark 后 | 1.99 / 2.31 / 2.33 | 可用 8.34 GB / 共 32.00 GB | 3.35 GB |

### 6.4 外推边界

- 全部数据来自单台主机的本机回环（127.0.0.1）：无网络损耗、无丢包、无跨机抖动，绝对延迟数值不可外推到公网、Wi-Fi、跨机房或弱网环境。
- 服务器与客户端同机共享 CPU / 内存，资源竞争模式与生产分机部署不同。
- 弱网与跨机结论必须以独立 Linux 网关 `netem.sh`（`BENCH_NETWORK` 标记实际环境）的结果为准。

## 7. 弱网与后续要求

弱网测试应在独立 Linux 网关使用 `netem.sh`，并通过 `BENCH_NETWORK` 标记实际环境。CPU 型号、同机/分机拓扑、Release 编译参数、git commit 与加密开关已在第 6 节记录；网卡实际流量计数器本次未留存，正式对外报告应补充系统网络计数器数据。
