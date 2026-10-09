#!/usr/bin/env bash
# Ork Python Demo — 一键环境安装（macOS / Linux）
#
# 步骤: 创建 .venv → 安装依赖 → 编译 KCP bridge → 运行离线自检
# 用法: cd python && bash setup.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Ork Python Demo 环境安装 ==="

# 0. 检查 python3（需要 3.10+：库使用 X | Y 类型语法）
if ! command -v python3 >/dev/null 2>&1; then
    echo "错误: 未找到 python3，请先安装 Python 3.10+" >&2
    exit 1
fi

# 1. 创建 venv（已存在时幂等）
echo "[1/4] 创建 Python 虚拟环境 (.venv)..."
python3 -m venv .venv
# 老版本 venv 的 activate 脚本与 set -u 不兼容，激活期间临时关闭
set +u
# shellcheck disable=SC1091
source .venv/bin/activate
set -u
python -m pip install --upgrade pip >/dev/null 2>&1 || true

# 2. 安装依赖（直连失败/超时则回退清华镜像）
echo "[2/4] 安装 Python 依赖..."
if ! pip install -r requirements.txt --timeout 15 2>/dev/null; then
    echo "  直连失败或超时，切换到清华镜像..."
    pip install -r requirements.txt \
        -i https://pypi.tuna.tsinghua.edu.cn/simple \
        --trusted-host pypi.tuna.tsinghua.edu.cn
fi

# 3. 编译 KCP bridge（libkcp_bridge.dylib / .so）
echo "[3/4] 编译 KCP bridge..."
if [[ -f lib/kcp/build.sh ]]; then
    bash lib/kcp/build.sh
else
    echo "  错误: lib/kcp/build.sh 不存在" >&2
    exit 1
fi

# 4. 离线自检：帧编解码向量 + 密码层 seal/open + KCP bridge 加载
echo "[4/4] 运行离线自检..."
python - <<'PY'
import sys

checks = []

def check(name, fn):
    try:
        fn()
        checks.append((name, "OK"))
    except Exception as exc:  # noqa: BLE001
        checks.append((name, f"FAIL ({exc})"))

# PyNaCl（加密后端）
def _nacl():
    import nacl.bindings  # noqa: F401
check("PyNaCl", _nacl)

# pytest
def _pytest():
    import pytest  # noqa: F401
check("pytest", _pytest)

# ork_frame 向量组 A/B 自检（Bootstrap V2 109B + Handshake V2 42B）
def _frame():
    import lib.ork_frame as f
    f._self_test()
check("ork_frame self-test", _frame)

# ork_cipher seal/open 往返自检
def _cipher():
    import lib.ork_cipher as c
    c._smoke_test()
check("ork_cipher smoke-test", _cipher)

# KCP bridge（实际 ctypes 加载验证）
def _kcp():
    from lib.kcp.ikcp_binding import RudpSession  # noqa: F401
check("KCP bridge", _kcp)

# 会话模块可导入（依赖 KCP bridge）
def _session():
    from lib.ork_client_session import OrkClientSession  # noqa: F401
check("OrkClientSession import", _session)

print()
for name, status in checks:
    symbol = "✓" if status == "OK" else "✗"
    print(f"  {symbol} {name}: {status}")

failed = [c for c in checks if c[1] != "OK"]
if failed:
    print(f"\n错误: {len(failed)} 项自检未通过", file=sys.stderr)
    sys.exit(1)
print("\n=== 环境准备完成 ===")
print("激活环境:  source .venv/bin/activate")
print("启动服务端: bash ../server/start.sh   (默认端口 9400)")
print("运行演示:  python demos/01_echo/client.py")
print("运行测试:  pytest tests/ -v")
PY
