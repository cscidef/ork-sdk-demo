#!/usr/bin/env bash
# Harmonia Ork 压力测试 — 客户端启动脚本（通用，支持多节点）
# 用法: ./run.sh --node <name> [--server-host IP] [--quick] [--clients-per-worker N] [--workers N]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# ─── 常量 ────────────────────────────────────────────────────────────────────
STRESS_DIR="$REPO_ROOT/stress"
STRESS_CONFIG="$STRESS_DIR/stress_config.yaml"
WORKER_SCRIPT="$STRESS_DIR/worker.py"
PYTHON_DIR="$REPO_ROOT/python"
VENV_PYTHON="$PYTHON_DIR/.venv/bin/python"
KCP_BRIDGE="$PYTHON_DIR/lib/kcp/libkcp_bridge.so"

# ─── 从 stress_config.yaml 读取默认值（去硬编码）─────────────────────────────
_cfg_val() {
  awk -v sec="$2" -v key="$3" '
    /^[a-z]/ { cur_sec=$1; sub(/:$/, "", cur_sec) }
    cur_sec == sec && $1 == key":" { print $2; exit }
  ' "$1" 2>/dev/null
}

_cfg_node_val() {
  # 从 nodes 列表中按 name 提取字段
  awk -v name="$2" -v key="$3" '
    /^[[:space:]]*- name:/ {
      gsub(/^[[:space:]]*- name:[[:space:]]*/, "")
      cur_name = $0
    }
    cur_name == name && $1 == key":" { print $2; exit }
  ' "$1" 2>/dev/null
}

if [[ -f "$STRESS_CONFIG" ]]; then
  _DEF_SERVER_HOST=$(_cfg_val "$STRESS_CONFIG" "server" "host")
  _DEF_SERVER_PORT=$(_cfg_val "$STRESS_CONFIG" "server" "port")
fi
_DEF_SERVER_HOST=${_DEF_SERVER_HOST:-127.0.0.1}
_DEF_SERVER_PORT=${_DEF_SERVER_PORT:-37564}

# ─── 默认参数 ────────────────────────────────────────────────────────────────
SERVER_HOST=""
SERVER_PORT=""
NODE_NAME=""
WORKERS=""
CLIENTS_PER_WORKER=""
DURATION_HOURS=24
QUICK=false

# ─── 参数解析 ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --server-host)        SERVER_HOST="$2"; shift 2 ;;
    --server-port)        SERVER_PORT="$2"; shift 2 ;;
    --node)               NODE_NAME="$2"; shift 2 ;;
    --quick)              QUICK=true; shift ;;
    --clients-per-worker) CLIENTS_PER_WORKER="$2"; shift 2 ;;
    --workers)            WORKERS="$2"; shift 2 ;;
    --duration-hours)     DURATION_HOURS="$2"; shift 2 ;;
    -h|--help)
      echo "用法: $0 --node <name> [选项]"
      echo ""
      echo "必选:"
      echo "  --node NAME             节点名称 (对应 stress_config.yaml 中 nodes[].name)"
      echo ""
      echo "可选:"
      echo "  --server-host IP        服务端地址 (默认: 从 stress_config.yaml 读取)"
      echo "  --server-port PORT      服务端端口 (默认: 从 stress_config.yaml 读取)"
      echo "  --quick                 快速验证模式 (1 worker × 5 clients, 5min)"
      echo "  --workers N             Worker 进程数 (默认: 从 stress_config.yaml 读取)"
      echo "  --clients-per-worker N  每 Worker 客户端数 (默认: 从 stress_config.yaml 读取)"
      echo "  --duration-hours N      测试持续小时数 (默认: 24)"
      exit 0
      ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

# ─── 校验必选参数 ────────────────────────────────────────────────────────────
if [[ -z "$NODE_NAME" ]]; then
  echo "❌ 错误: 必须指定 --node <name>"
  echo "   可用节点请查看: $STRESS_CONFIG 中 nodes[].name"
  exit 1
fi

# ─── 从配置中填充未指定的参数 ─────────────────────────────────────────────────
SERVER_HOST=${SERVER_HOST:-$_DEF_SERVER_HOST}
SERVER_PORT=${SERVER_PORT:-$_DEF_SERVER_PORT}

if [[ -z "$WORKERS" && -f "$STRESS_CONFIG" ]]; then
  WORKERS=$(_cfg_node_val "$STRESS_CONFIG" "$NODE_NAME" "workers")
fi
WORKERS=${WORKERS:-5}

if [[ -z "$CLIENTS_PER_WORKER" && -f "$STRESS_CONFIG" ]]; then
  CLIENTS_PER_WORKER=$(_cfg_node_val "$STRESS_CONFIG" "$NODE_NAME" "clients_per_worker")
fi
CLIENTS_PER_WORKER=${CLIENTS_PER_WORKER:-50}

if [[ "$QUICK" == true ]]; then
  WORKERS=1
  CLIENTS_PER_WORKER=5
  DURATION_HOURS=0.083  # ~5 minutes
fi

# ─── 派生变量 ────────────────────────────────────────────────────────────────
RUN_ID="stress_$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="$STRESS_DIR/results/$RUN_ID"
TOTAL_CLIENTS=$((WORKERS * CLIENTS_PER_WORKER))

# ─── 前置检查 ────────────────────────────────────────────────────────────────
echo "━━━ Harmonia Ork 压力测试 — 客户端 ($NODE_NAME) ━━━"
echo ""
echo "  服务端:     $SERVER_HOST:$SERVER_PORT"
echo "  Workers:    $WORKERS"
echo "  每 Worker:  $CLIENTS_PER_WORKER 客户端"
echo "  总计:       $TOTAL_CLIENTS 客户端"
echo "  持续时间:   ${DURATION_HOURS}h"
echo ""

# 1. 检测 Python 环境
if [[ ! -f "$VENV_PYTHON" ]]; then
  echo "❌ 错误: Python 虚拟环境不存在: $VENV_PYTHON"
  echo ""
  echo "请先创建虚拟环境:"
  echo "  cd $PYTHON_DIR"
  echo "  python3 -m venv .venv"
  echo "  source .venv/bin/activate"
  echo "  pip install -r requirements.txt"
  echo ""
  exit 1
fi
echo "✓ Python 环境: $VENV_PYTHON"

# 2. 检测 OrkClientSession 可导入
if ! "$VENV_PYTHON" -c "import sys; sys.path.insert(0, '$PYTHON_DIR'); from client.ork_client_session import OrkClientSession" 2>/dev/null; then
  echo "❌ 错误: 无法导入 OrkClientSession"
  echo ""
  echo "请检查:"
  echo "  1. demo/python/.venv 已创建"
  echo "  2. requirements.txt 已安装: pip install -r requirements.txt"
  echo "  3. generated/ 目录包含 proto 生成文件"
  echo ""
  exit 1
fi
echo "✓ OrkClientSession 可导入"

# 3. 检测 KCP bridge
if [[ ! -f "$KCP_BRIDGE" ]]; then
  # macOS 上使用 .dylib
  KCP_BRIDGE_DYLIB="$PYTHON_DIR/lib/kcp/libkcp_bridge.dylib"
  if [[ -f "$KCP_BRIDGE_DYLIB" ]]; then
    KCP_BRIDGE="$KCP_BRIDGE_DYLIB"
  else
    echo "❌ 错误: KCP bridge 不存在: $KCP_BRIDGE"
    echo ""
    echo "请编译 KCP bridge:"
    echo "  cd $PYTHON_DIR"
    echo "  bash lib/kcp/build.sh"
    echo ""
    exit 1
  fi
fi
echo "✓ KCP bridge: $KCP_BRIDGE"

# 4. 提升 fd 上限
if ! ulimit -n 65535 2>/dev/null; then
  echo "⚠️  警告: 无法设置 ulimit -n 65535，当前值: $(ulimit -n)"
  echo "   建议手动执行: ulimit -n 65535"
  echo "   或添加到 /etc/security/limits.conf:"
  echo "     * soft nofile 65535"
  echo "     * hard nofile 65535"
else
  echo "✓ fd 上限: $(ulimit -n)"
fi

# 5. 检测服务端可达性
echo -n "   检测服务端 $SERVER_HOST:$SERVER_PORT"
if nc -z -w 3 "$SERVER_HOST" "$SERVER_PORT" 2>/dev/null || timeout 3 bash -c "echo > /dev/tcp/$SERVER_HOST/$SERVER_PORT" 2>/dev/null; then
  echo " ✓"
else
  echo " ⚠️  无法连接（服务端可能未启动）"
  echo "   将继续启动，客户端会自动重试连接"
fi

# ─── 创建结果目录 ────────────────────────────────────────────────────────────
mkdir -p "$RESULTS_DIR"
echo ""
echo "✓ 结果目录: $RESULTS_DIR"

# ─── 清理函数 ────────────────────────────────────────────────────────────────
WORKER_PIDS=()

cleanup() {
  echo ""
  echo "─── 清理 ───"
  for pid in "${WORKER_PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      echo "停止 worker PID=$pid..."
      kill "$pid" 2>/dev/null || true
    fi
  done
  # 等待所有 worker 退出
  for pid in "${WORKER_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  echo "✓ 清理完成"
}
trap cleanup EXIT INT TERM

# ─── 启动 Worker 进程 ────────────────────────────────────────────────────────
echo ""
echo "─── 启动 $WORKERS 个 Worker 进程 ───"

for i in $(seq 0 $((WORKERS - 1))); do
  WORKER_LOG="$RESULTS_DIR/worker_${i}.log"

  "$VENV_PYTHON" "$WORKER_SCRIPT" \
    --config "$STRESS_CONFIG" \
    --node "$NODE_NAME" \
    --shard "$i" \
    --shard-count "$WORKERS" \
    > "$WORKER_LOG" 2>&1 &

  WORKER_PID=$!
  WORKER_PIDS+=("$WORKER_PID")
  echo "  ✓ Worker $i: PID=$WORKER_PID, 日志=$WORKER_LOG"

  # 错开启动，避免瞬间连接风暴
  if [[ $i -lt $((WORKERS - 1)) ]]; then
    sleep 2
  fi
done

echo ""
echo "━━━ 客户端启动完成 ━━━"
echo ""
echo "  Run ID:       $RUN_ID"
echo "  节点:         $NODE_NAME"
echo "  总客户端:     $TOTAL_CLIENTS"
echo "  Worker PIDs:  ${WORKER_PIDS[*]}"
echo "  结果目录:     $RESULTS_DIR"
echo ""
echo "  查看日志:     tail -f $RESULTS_DIR/worker_*.log"
echo "  查看指标:     tail -f $RESULTS_DIR/*.jsonl"
echo ""
echo "  按 Ctrl+C 停止所有 Worker"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# ─── 等待 Worker 完成 ────────────────────────────────────────────────────────
# 显示第一个 worker 的实时日志
echo "─── Worker 0 实时日志 ───"
tail -f "$RESULTS_DIR/worker_0.log" &
TAIL_PID=$!

# 等待所有 worker 退出
wait "${WORKER_PIDS[@]}" 2>/dev/null || true

# 停止 tail
kill "$TAIL_PID" 2>/dev/null || true

echo ""
echo "━━━ 所有 Worker 已退出 ━━━"
echo ""
echo "结果保存在: $RESULTS_DIR"
echo ""
echo "在服务端上生成报告:"
echo "  python stress/generate_stress_report.py \\"
 echo "    --results stress/results/$RUN_ID \\"
 echo "    --output stress/reports/$RUN_ID.md \\"
 echo "    --config stress/stress_config.yaml"
