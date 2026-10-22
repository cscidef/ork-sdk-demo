#!/usr/bin/env bash
# Harmonia Ork 压力测试 — 服务端启动脚本
# 用法: ./run.sh [--quick] [--duration-hours N]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# ─── 参数解析 ────────────────────────────────────────────────────────────────
QUICK=false
DURATION_HOURS=24

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick)          QUICK=true; shift ;;
    --duration-hours) DURATION_HOURS="$2"; shift 2 ;;
    -h|--help)
      echo "用法: $0 [--quick] [--duration-hours N]"
      echo "  --quick           5分钟快速验证模式"
      echo "  --duration-hours  测试持续小时数（默认 24）"
      exit 0
      ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

if [[ "$QUICK" == true ]]; then
  DURATION_HOURS=0.083  # ~5 minutes
fi

# ─── 常量 ────────────────────────────────────────────────────────────────────
# 服务端使用 ork-sdk-demo 分发包的 embed_minimal（平台自适应）
_os="$(uname -s)"; _arch="$(uname -m)"
case "${_os}-${_arch}" in
  Darwin-arm64) PLATFORM="macos-arm64" ;;
  Linux-x86_64) PLATFORM="linux-x64" ;;
  *) echo "❌ 不支持的平台: ${_os}-${_arch}（见 server/README.md）"; exit 1 ;;
esac
ORK_BIN="$REPO_ROOT/server/${PLATFORM}/bin/embed_minimal"
CONFIG_DIR="$REPO_ROOT/server/config"
LOG_DIR="$REPO_ROOT/server/logs"
STRESS_DIR="$REPO_ROOT/stress"
STRESS_CONFIG="$STRESS_DIR/stress_config.yaml"
OVERLAY_DIR="$SCRIPT_DIR/.run"
OVERLAY_CONFIG="$OVERLAY_DIR/config"
MONITOR_SCRIPT="$STRESS_DIR/monitor_server.py"
RUN_ID="stress_$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="$STRESS_DIR/results/$RUN_ID"

# ─── 从 stress_config.yaml 读取网络参数（去硬编码）────────────────────────────
_cfg_val() {
  awk -v sec="$2" -v key="$3" '
    /^[a-z]/ { cur_sec=$1; sub(/:$/, "", cur_sec) }
    cur_sec == sec && $1 == key":" { print $2; exit }
  ' "$1" 2>/dev/null
}

if [[ -f "$STRESS_CONFIG" ]]; then
  SERVER_PORT=$(_cfg_val "$STRESS_CONFIG" "server" "port")
  SERVER_HOST=$(_cfg_val "$STRESS_CONFIG" "server" "host")
  MONITOR_PORT=$(_cfg_val "$STRESS_CONFIG" "monitoring" "port")
fi
SERVER_PORT=${SERVER_PORT:-37564}
SERVER_HOST=${SERVER_HOST:-0.0.0.0}
MONITOR_PORT=${MONITOR_PORT:-39000}

# ─── 前置检查 ────────────────────────────────────────────────────────────────
echo "━━━ Harmonia Ork 压力测试 — 服务端 ━━━"
echo ""

# 1. 检测 ork_room_server 二进制
if [[ ! -f "$ORK_BIN" ]]; then
  echo "❌ 错误: 服务端二进制不存在: $ORK_BIN"
  echo ""
  echo "请先打包服务端分发包:"
  echo "  在 harmonia_ork 源仓执行 bash scripts/pack_server_macos.sh（macOS）"
  echo "  或 bash scripts/pack_server.sh（Linux），产物同步到 server/${PLATFORM}/"
  echo ""
  exit 1
fi
echo "✓ 服务端二进制 (embed_minimal): $ORK_BIN"

# 2. 提升 fd 上限
if ! ulimit -n 8192 2>/dev/null; then
  echo "⚠️  警告: 无法设置 ulimit -n 8192，当前值: $(ulimit -n)"
  echo "   建议手动执行: ulimit -n 8192"
else
  echo "✓ fd 上限: $(ulimit -n)"
fi

# 3. 生成配置覆盖层（从仓库 config/ 复制 + 注入 stress_config 覆盖值）
echo ""
echo "─── 生成配置覆盖层 ───"

rm -rf "$OVERLAY_DIR"
mkdir -p "$OVERLAY_CONFIG"
mkdir -p "$LOG_DIR"
mkdir -p "$RESULTS_DIR"

# 复制仓库默认档
cp "$CONFIG_DIR"/*.yaml "$OVERLAY_CONFIG/"
# 将 log.yaml 中的相对路径改为绝对路径，避免 cwd 切换后日志写错位置
sed -i.bak "s|^\([[:space:]]*file:\).*|\1 $LOG_DIR/ork.log|" "$OVERLAY_CONFIG/log.yaml"
rm -f "$OVERLAY_CONFIG/log.yaml.bak"
echo "✓ 已复制仓库默认配置到 $OVERLAY_CONFIG"

# 从 stress_config.yaml 读取覆盖值并注入
if [[ -f "$STRESS_CONFIG" ]]; then
  echo "✓ 读取压测配置: $STRESS_CONFIG"

  # 用纯 sed 解析 YAML config_overrides 节，输出 key|file|value 三元组
  while IFS='|' read -r key file value; do
    target="$OVERLAY_CONFIG/$file"
    if [[ -f "$target" ]]; then
      sed -i.bak "s/^\([[:space:]]*\)${key}:.*/\1${key}: ${value}/" "$target"
      rm -f "${target}.bak"
      echo "  ✓ 覆盖 ${file} :: ${key} = ${value}"
    else
      echo "  ⚠️  目标文件不存在，跳过: $file"
    fi
  done < <(
    sed -n -e '/config_overrides:/,/^[^ ]/{' \
           -e '/^[^ ]/d' \
           -e '/config_overrides:/d' \
           -e '/^[[:space:]]*#/d' \
           -e '/^[[:space:]]*$/d' \
           -e 'p' \
           -e '}' "$STRESS_CONFIG" \
    | sed -E 's/^[[:space:]]*([a-z_]+)\.([a-z_]+):[[:space:]]*(.*)/\2|\1.yaml|\3/'
  )
else
  echo "⚠️  未找到 $STRESS_CONFIG，使用仓库默认配置（无覆盖）"
fi

# 打印最终生效值
echo ""
echo "─── 覆盖层最终值 ───"
echo "  room.max_players:          $(grep -E '^\s*max_players:' "$OVERLAY_CONFIG/room.yaml" | awk '{print $2}')"
echo "  network.bootstrap_ttl_sec: $(grep -E '^\s*bootstrap_ttl_sec:' "$OVERLAY_CONFIG/network.yaml" | awk '{print $2}')"
echo "  log.level:                 $(grep -E '^\s*level:' "$OVERLAY_CONFIG/log.yaml" | awk '{print $2}' | tr -d '#')"

# ─── 启动 ork_room_server ────────────────────────────────────────────────────────
echo ""
echo "─── 启动 ork_room_server ───"

# workspace 指向覆盖层目录，使 ConfigManager 读取覆盖后的 config/
"$ORK_BIN" --gamePort "$SERVER_PORT" --workspace "$OVERLAY_DIR" > "$LOG_DIR/ork_server_stdout.log" 2>&1 &
ORK_PID=$!
echo "✓ ork server (embed_minimal) PID=$ORK_PID, 日志: $LOG_DIR/ork.log"

# 等待端口就绪
echo -n "   等待端口 $SERVER_PORT 就绪"
for i in $(seq 1 30); do
  if nc -z 127.0.0.1 "$SERVER_PORT" 2>/dev/null || lsof -i :"$SERVER_PORT" >/dev/null 2>&1; then
    echo " ✓"
    break
  fi
  if ! kill -0 "$ORK_PID" 2>/dev/null; then
    echo " ✗"
    echo "❌ ork server 启动失败，查看日志: $LOG_DIR/ork_server_stdout.log"
    exit 1
  fi
  echo -n "."
  sleep 1
done

if ! nc -z 127.0.0.1 "$SERVER_PORT" 2>/dev/null && ! lsof -i :"$SERVER_PORT" >/dev/null 2>&1; then
  echo ""
  echo "❌ 端口 $SERVER_PORT 未就绪，超时退出"
  kill "$ORK_PID" 2>/dev/null || true
  exit 1
fi

# ─── 清理函数 ────────────────────────────────────────────────────────────────
cleanup() {
  echo ""
  echo "─── 清理 ───"
  if [[ -n "${MONITOR_PID:-}" ]] && kill -0 "$MONITOR_PID" 2>/dev/null; then
    echo "停止 monitor_server (PID=$MONITOR_PID)..."
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
  fi
  if kill -0 "$ORK_PID" 2>/dev/null; then
    echo "停止 ork server (PID=$ORK_PID)..."
    kill "$ORK_PID" 2>/dev/null || true
    wait "$ORK_PID" 2>/dev/null || true
  fi
  echo "✓ 清理完成"
}
trap cleanup EXIT INT TERM

# ─── 启动 monitor_server ────────────────────────────────────────────────────
echo ""
echo "─── 启动 monitor_server ───"

# 确保 Python 环境
PYTHON_CMD="python3"
VENV_PYTHON="$REPO_ROOT/python/.venv/bin/python"
if [[ -f "$VENV_PYTHON" ]]; then
  PYTHON_CMD="$VENV_PYTHON"
fi

echo "使用 Python: $PYTHON_CMD"
echo "Run ID: $RUN_ID"
echo "结果目录: $RESULTS_DIR"
echo ""

# 前台启动 monitor_server（显示仪表盘）
"$PYTHON_CMD" "$MONITOR_SCRIPT" \
  --config "$STRESS_CONFIG" \
  --run-id "$RUN_ID" \
  --ork-pid "$ORK_PID" &
MONITOR_PID=$!

echo "✓ monitor_server PID=$MONITOR_PID"
echo ""
echo "━━━ 服务端就绪 ━━━"
echo ""
echo "  Run ID:       $RUN_ID"
echo "  ork server:      PID=$ORK_PID, 端口 $SERVER_PORT"
echo "  monitor:      PID=$MONITOR_PID, 端口 $MONITOR_PORT"
echo "  持续时间:     ${DURATION_HOURS}h"
echo "  结果目录:     $RESULTS_DIR"
echo ""
echo "  客户端机器请连接: $SERVER_HOST:$SERVER_PORT"
echo ""
echo "  按 Ctrl+C 停止所有服务"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# 等待 monitor 进程（前台显示仪表盘）
wait "$MONITOR_PID"
