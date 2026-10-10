#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
if [[ -d "${REPO_ROOT}/python/.venv" ]]; then
    cd "${REPO_ROOT}/python"
    if .venv/bin/python -m pytest tests/ -v --tb=short; then
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
# No pipes here on purpose: `cmd | tail` makes the `if` test tail's exit status
# (always 0), so a configure/build/test failure would silently count as a pass.
echo "--- C++ Tests ---"
cd "${REPO_ROOT}/cpp"
if command -v cmake &>/dev/null; then
    if ! cmake -B build -DCMAKE_BUILD_TYPE=Debug; then
        FAIL=$((FAIL + 1))
        echo "[FAIL] C++ configure"
    else
        # Never expand to a bare -j: that would mean unlimited parallelism.
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
        if ! cmake --build build -j"${JOBS}"; then
            FAIL=$((FAIL + 1))
            echo "[FAIL] C++ build"
        else
            cd build
            if ctest --output-on-failure; then
                PASS=$((PASS + 1))
                echo "[PASS] C++ tests"
            else
                FAIL=$((FAIL + 1))
                echo "[FAIL] C++ tests"
            fi
        fi
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

[[ "$FAIL" -eq 0 ]] || exit 1
