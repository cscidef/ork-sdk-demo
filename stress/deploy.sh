#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

# ─── Argument parsing ──────────────────────────────────────────────────────────
CONFIG_FILE=""
HOSTS=()
USERS=()
WORK_DIRS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)   CONFIG_FILE="$2"; shift 2 ;;
    --host)     HOSTS+=("$2"); shift 2 ;;
    --user)     USERS+=("$2"); shift 2 ;;
    --work-dir) WORK_DIRS+=("$2"); shift 2 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

# Parse nodes from YAML if config is provided
if [[ -n "$CONFIG_FILE" ]]; then
  while IFS='|' read -r name host user work_dir; do
    HOSTS+=("$host")
    USERS+=("$user")
    WORK_DIRS+=("$work_dir")
  done < <(python3 -c "
import yaml, sys
cfg = yaml.safe_load(open('$CONFIG_FILE'))
for n in cfg.get('nodes', []):
    print(f\"{n['name']}|{n['host']}|{n['ssh_user']}|{n['work_dir']}\")
")
fi

if [[ ${#HOSTS[@]} -eq 0 ]]; then
  echo "Error: no nodes specified. Use --config or --host/--user/--work-dir"
  exit 1
fi

# ─── Deploy to each node ───────────────────────────────────────────────────────
RSYNC_EXCLUDES=(
  --exclude '.venv' --exclude 'build' --exclude 'logs'
  --exclude '__pycache__' --exclude '.git' --exclude '*.dylib'
)

for i in "${!HOSTS[@]}"; do
  host="${HOSTS[$i]}"
  user="${USERS[$i]}"
  work_dir="${WORK_DIRS[$i]}"
  remote="${user}@${host}"
  dest="$work_dir"

  echo "━━━ Deploying to ${remote}:${work_dir} ━━━"

  # 1. Sync repository
  rsync -az "${RSYNC_EXCLUDES[@]}" "$repo_root/" "${remote}:${dest}/"

  # 2. Ensure python3 + venv
  ssh "$remote" "cd $dest && bash -s" <<'EOF'
set -euo pipefail
command -v python3 >/dev/null || { echo "ERROR: python3 not found"; exit 1; }
VENV="python/.venv"
if [ ! -d "$VENV" ]; then
  python3 -m venv "$VENV"
  echo "Created venv at $VENV"
else
  echo "Venv already exists, skipping"
fi
EOF

  # 3. Install dependencies
  ssh "$remote" "cd $dest && bash -s" <<'EOF'
set -euo pipefail
source python/.venv/bin/activate
pip install -q -r python/requirements.txt
EOF

  # 4. Build KCP shared library (idempotent)
  ssh "$remote" "cd $dest && bash -s" <<'EOF'
set -euo pipefail
SO="python/lib/kcp/libkcp_bridge.so"
if [ -f "$SO" ]; then
  echo "libkcp_bridge.so exists, skipping build"
else
  cd python && bash lib/kcp/build.sh
  echo "Built $SO"
fi
EOF

  # 5. Verify import
  ssh "$remote" "cd $dest && bash -s" <<'EOF'
set -euo pipefail
source demo/python/.venv/bin/activate
python -c "import sys; sys.path.insert(0,'python'); from client.ork_client_session import OrkClientSession; print('Import OK')"
EOF

  # 6. Ensure ulimit
  ssh "$remote" bash -s <<'EOF'
set -euo pipefail
LINE="ulimit -n 65535"
if ! grep -qF "$LINE" ~/.bashrc 2>/dev/null; then
  echo "$LINE" >> ~/.bashrc
  echo "Added ulimit to ~/.bashrc"
else
  echo "ulimit already configured"
fi
EOF

  echo "✓ ${remote} deployed successfully"
done

echo "━━━ All nodes deployed ━━━"
