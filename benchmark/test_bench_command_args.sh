#!/usr/bin/env bash
# test_bench_command_args.sh — 验证 run_benchmark.sh 保留原始 CLI 参数到 metadata
# 不运行 benchmark 或重型构建，仅测试 shell_quote 和 original_args 机制。
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_script="${script_dir}/run_benchmark.sh"

# 从 run_benchmark.sh 提取 shell_quote 函数定义
eval "$(sed -n '/^shell_quote()/,/^}/p' "${source_script}")"

pass=0
fail=0
assert_eq() {
    local label="$1" expected="$2" actual="$3"
    if [[ "${expected}" == "${actual}" ]]; then
        echo "  PASS: ${label}"
        pass=$((pass + 1))
    else
        echo "  FAIL: ${label}"
        echo "    expected: '${expected}'"
        echo "    actual:   '${actual}'"
        fail=$((fail + 1))
    fi
}

echo "=== Test 1: shell_quote 无参数 ==="
result="$(shell_quote)"
assert_eq "empty input → empty output" "" "${result}"

echo "=== Test 2: shell_quote 单个普通参数 ==="
result="$(shell_quote "--quick")"
assert_eq "simple flag" "--quick" "${result}"

echo "=== Test 3: shell_quote 多个参数 ==="
result="$(shell_quote --quick --clients 5 --duration 15)"
assert_eq "multiple args" "--quick --clients 5 --duration 15" "${result}"

echo "=== Test 4: shell_quote 含空格参数 ==="
result="$(shell_quote --name "hello world")"
assert_eq "space in arg" "--name hello\\ world" "${result}"

echo "=== Test 5: shell_quote 含特殊字符 ==="
result="$(shell_quote 'a$b' "c'd")"
# printf %q 会转义 $ 和 '
# 验证转义后的值可以被 shell 还原
eval "restored=(${result})"
assert_eq "special chars round-trip count" "2" "${#restored[@]}"
assert_eq "special chars round-trip[0]" 'a$b' "${restored[0]}"
assert_eq "special chars round-trip[1]" "c'd" "${restored[1]}"

echo "=== Test 6: original_args 在 while/shift 后保留 ==="
# 模拟 run_benchmark.sh 的参数保存模式
simulate() {
    local original_args=("$@")
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --quick) shift ;;
            --clients) shift 2 ;;
            *) shift ;;
        esac
    done
    # 此时 $@ 已空，但 original_args 保留
    if [[ ${#original_args[@]} -gt 0 ]]; then
        shell_quote "${original_args[@]}"
    fi
}
result="$(simulate --quick --clients 5)"
assert_eq "preserved after shift" "--quick --clients 5" "${result}"

echo "=== Test 7: original_args 无参数场景 ==="
result="$(simulate)"
assert_eq "no args preserved" "" "${result}"

echo ""
echo "Results: ${pass} passed, ${fail} failed"
[[ ${fail} -eq 0 ]] || exit 1
echo "All tests passed."
