#!/usr/bin/env bash
#
# check_platform.sh — Detect the host OS/arch and print the matching server
# platform directory name under server/ (e.g. "macos-arm64").
#
# Exit codes:
#   0  platform supported (name printed on stdout)
#   1  platform not supported (message on stderr)
#
set -euo pipefail

OS="$(uname -s)"
ARCH="$(uname -m)"

case "${OS}-${ARCH}" in
    Darwin-arm64)  PLATFORM="macos-arm64" ;;
    Darwin-x86_64) PLATFORM="" ; echo "ERROR: macOS x86_64 is not supported (arm64 only)." >&2 ; exit 1 ;;
    Linux-x86_64)  PLATFORM="linux-x64" ;;
    Linux-aarch64) PLATFORM="" ; echo "ERROR: Linux arm64 is not supported yet." >&2 ; exit 1 ;;
    *)             PLATFORM="" ; echo "ERROR: Unsupported platform: ${OS}-${ARCH}" >&2 ; exit 1 ;;
esac

echo "$PLATFORM"
