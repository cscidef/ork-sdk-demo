#!/usr/bin/env bash
# Harmonia Ork 压力测试 — SSH 自动编排脚本（可选）
# 从 Mac(225) 通过 SSH 启动两台 Linux 的 run.sh，然后本地启动 server+monitor
# 用法: ./coordinator.sh [--config stress_config.yaml] [--quick] [--dry-run]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ─── 默认参数 ────────────────────────────────────────────────────────────────
CONFIG_FILE="$SCRIPT_DIR/stress_config.yaml"
QUICK=false
DRY_RUN=false

# ─── 参数解析 ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)   CONFIG_FILE="$2"; shift 2 ;;
    --quick)    QUICK=true; shift ;;
    --dry-run)  DRY_RUN=true; shift ;;
    -h|--help)
      echo "用法: $0 [选项]"
      echo "  --config PATH   stress_config.yaml 路径 (默认: 同目录下)"
      echo "  --quick         快速验证模式"
      echo "  --dry-run       只打印命令，不实际执行"
      exit 0
      ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

# ─── 从 YAML 解析节点配置 ────────────────────────────────────────────────────
if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "❌ 错误: 配置文件不存在: $CONFIG_FILE"
  exit 1
fi

# 解析节点信息
readarray -t NODES < <(python3 -c "
import yaml
cfg = yaml.safe_load(open('$CONFIG_FILE'))
for n in cfg.get('nodes', []):
    print(f\"{n['name']}|{n['host']}|{n['ssh_user']}|{n.get('work_dir', '~/ork-sdk-demo')}\")
")

if [[ ${#NODES[@]} -eq 0 ]]; then
  echo "❌ 错误: 配置文件中未找到 nodes 定义"
  exit 1
fi

echo "━━━ Harmonia Ork 压力测试 — SSH 自动编排 ━━━"
echo ""
echo "  配置文件: $CONFIG_FILE"
echo "  快速模式: $QUICK"
echo "  Dry Run:  $DRY_RUN"
echo ""
echo "  节点列表:"
for node in "${NODES[@]}"; do
  IFS='|' read -r name host user work_dir <<< "$node"
  echo "    - $name ($user@$host:$work_dir)"
done
echo ""

# ─── SSH 命令构建 ────────────────────────────────────────────────────────────
QUICK_FLAG=""
if [[ "$QUICK" == true ]]; then
  QUICK_FLAG="--quick"
fi

# ─── 启动远程客户端节点 ──────────────────────────────────────────────────────
REMOTE_PIDS=()

start_remote_node() {
  local name="$1"
  local host="$2"
  local user="$3"
  local work_dir="$4"

  local remote="${user}@${host}"
  local package_dir="$work_dir/stress/package_client"

  # 构建远程命令
  local cmd="cd $package_dir && ./run.sh $QUICK_FLAG --node $name"

  echo "─── 启动 $name ($remote) ───"

  if [[ "$DRY_RUN" == true ]]; then
    echo "  [DRY RUN] ssh $remote '$cmd'"
    return 0
  fi

  # 通过 SSH 后台启动
  ssh -f "$remote" "$cmd"
  echo "  ✓ 已发送启动命令到 $remote"
}

for node in "${NODES[@]}"; do
  IFS='|' read -r name host user work_dir <<< "$node"
  start_remote_node "$name" "$host" "$user" "$work_dir"
  sleep 1
done

echo ""

# ─── 启动本地服务端 ──────────────────────────────────────────────────────────
echo "─── 启动本地服务端 ───"

SERVER_PACKAGE="$SCRIPT_DIR/package_server"

if [[ "$DRY_RUN" == true ]]; then
  echo "  [DRY RUN] cd $SERVER_PACKAGE && ./run.sh $QUICK_FLAG"
  exit 0
fi

# 切换到服务端包目录并执行
cd "$SERVER_PACKAGE"
./run.sh $QUICK_FLAG
