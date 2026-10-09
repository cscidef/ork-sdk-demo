#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="${SCRIPT_DIR}/.server.pid"
LOG_FILE="${SCRIPT_DIR}/server.log"

# Detect platform
OS="$(uname -s)"
ARCH="$(uname -m)"
case "${OS}-${ARCH}" in
    Darwin-arm64) PLATFORM="macos-arm64" ;;
    Linux-x86_64) PLATFORM="linux-x64" ;;
    *) echo "ERROR: Unsupported platform: ${OS}-${ARCH}"; echo "See server/README.md for supported platforms."; exit 1 ;;
esac

BIN="${SCRIPT_DIR}/${PLATFORM}/bin/embed_minimal"

if [ ! -f "$BIN" ]; then
    echo "ERROR: Server binary not found: $BIN"
    echo "Run scripts/pack_server.sh first, or see server/${PLATFORM}/README.md"
    exit 1
fi

# Check if already running
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Server already running (PID $OLD_PID). Use stop.sh first."
        exit 1
    fi
    rm -f "$PID_FILE"
fi

# Check port availability
GAME_PORT=${GAME_PORT:-9400}
if lsof -i ":${GAME_PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "ERROR: Port ${GAME_PORT} is already in use."
    echo "Set GAME_PORT env var to use a different port."
    exit 1
fi

# The engine's log sink writes to <workspace>/logs/ork.log (see config/log.yaml);
# spdlog's file sink does not create parent directories, so make it here.
mkdir -p "${SCRIPT_DIR}/logs"

echo "Starting Ork demo server (${PLATFORM})..."
echo "  Binary: $BIN"
echo "  Port:   $GAME_PORT"
echo "  Log:    $LOG_FILE"

# Start server
"$BIN" --gamePort "$GAME_PORT" --workspace "${SCRIPT_DIR}" > "$LOG_FILE" 2>&1 &
SERVER_PID=$!
echo "$SERVER_PID" > "$PID_FILE"

# Wait briefly and check it's still alive
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "ERROR: Server failed to start. Check $LOG_FILE"
    rm -f "$PID_FILE"
    cat "$LOG_FILE"
    exit 1
fi

echo "Server started (PID $SERVER_PID)"
echo "  TCP bootstrap: localhost:${GAME_PORT}"
echo "  UDP/KCP:       localhost:${GAME_PORT}  (UDP shares the same port)"
