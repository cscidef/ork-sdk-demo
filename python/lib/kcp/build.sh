#!/usr/bin/env bash
# 编译 kcp_bridge.c + ikcp.c → 共享库
#   macOS  → libkcp_bridge.dylib
#   Linux  → libkcp_bridge.so
# 用法: cd python && bash lib/kcp/build.sh
#       （也可直接 cd python/lib/kcp && bash build.sh）
set -euo pipefail

OUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE="$OUT_DIR/kcp_bridge.c"
IKCP="$OUT_DIR/ikcp.c"
OUT="$OUT_DIR/libkcp_bridge"

# 选择编译器：优先 $CC，其次 clang，最后 cc
CC_BIN="${CC:-}"
if [[ -z "$CC_BIN" ]]; then
  if command -v clang >/dev/null 2>&1; then
    CC_BIN="clang"
  else
    CC_BIN="cc"
  fi
fi

for f in "$BRIDGE" "$IKCP"; do
  if [[ ! -f "$f" ]]; then
    echo "错误: 缺少源文件 $f" >&2
    exit 1
  fi
done

OS="$(uname -s)"
case "$OS" in
  Darwin)
    "$CC_BIN" -O2 -fPIC -shared -o "${OUT}.dylib" "$BRIDGE" "$IKCP"
    echo "Built ${OUT}.dylib  (compiler: $CC_BIN)"
    ;;
  Linux)
    "$CC_BIN" -O2 -fPIC -shared -o "${OUT}.so" "$BRIDGE" "$IKCP"
    echo "Built ${OUT}.so  (compiler: $CC_BIN)"
    ;;
  *)
    echo "错误: 不支持的平台 $OS（仅 macOS / Linux；Windows 请手动用 MSVC/MinGW 编译）" >&2
    exit 1
    ;;
esac

echo "  KCP_BRIDGE_MAX_SESSIONS 见 kcp_bridge.c（默认 256，须 >= 并发客户端数）"
