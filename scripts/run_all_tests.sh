#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=============================="
echo " Ork SDK Demo - Full Test Run"
echo "=============================="
echo ""

PASS=0
FAIL=0
SKIP=0

# --- Python Tests ---
echo "--- Python Tests ---"
if [ -d "${REPO_ROOT}/python/.venv" ]; then
    cd "${REPO_ROOT}/python"
    if .venv/bin/python -m pytest tests/ -v --tb=short 2>&1; then
        PASS=$((PASS + 1))
        echo "[PASS] Python tests"
    else
        FAIL=$((FAIL + 1))
        echo "[FAIL] Python tests"
    fi
else
    SKIP=$((SKIP + 1))
    echo "[SKIP] Python tests (run 'make setup' first)"
fi
echo ""

# --- C++ Tests ---
echo "--- C++ Tests ---"
cd "${REPO_ROOT}/cpp"
if command -v cmake &>/dev/null; then
    cmake -B build -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
    if cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -5; then
        cd build
        if ctest --output-on-failure 2>&1; then
            PASS=$((PASS + 1))
            echo "[PASS] C++ tests"
        else
            FAIL=$((FAIL + 1))
            echo "[FAIL] C++ tests"
        fi
    else
        FAIL=$((FAIL + 1))
        echo "[FAIL] C++ build"
    fi
else
    SKIP=$((SKIP + 1))
    echo "[SKIP] C++ tests (cmake not found)"
fi
echo ""

# --- Summary ---
echo "=============================="
echo " Summary: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
echo "=============================="

[ "$FAIL" -eq 0 ] || exit 1
