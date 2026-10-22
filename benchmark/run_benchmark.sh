#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${ORK_BUILD_DIR:-${repo_root}/cpp}"
reference_bin="${script_dir}/bin/reference_server"
python_bin="${repo_root}/python/.venv/bin/python"
clients=10
duration=15
repetitions=3
pose_hz=30
reliable_hz=5
payload_bytes=256
bone_count=8
reference_port=38664
ork_port="${ORK_PORT:-37564}"
quick=false
suite=false
client_levels=""
network="${BENCH_NETWORK:-unshaped}"
server_pid=""

usage() {
    echo "用法: $0 [--quick|--suite] [--clients N|--client-levels 1,10,20] [--duration SEC] [--repetitions N] [--pose-hz HZ] [--reliable-hz HZ] [--payload-bytes N] [--network unshaped|broadband|mobile|weak]"
}

# 保留原始 CLI 参数，while/shift 会消耗 $@
original_args=("$@")

# shell_quote: 对每个参数安全转义，兼容空格与特殊字符，不执行 eval
shell_quote() {
    local sep="" i
    for i in "$@"; do
        printf '%s%q' "$sep" "$i"
        sep=" "
    done
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick) quick=true; shift ;;
        --suite) suite=true; shift ;;
        --client-levels) client_levels="$2"; shift 2 ;;
        --clients) clients="$2"; shift 2 ;;
        --duration) duration="$2"; shift 2 ;;
        --repetitions) repetitions="$2"; shift 2 ;;
        --pose-hz) pose_hz="$2"; shift 2 ;;
        --reliable-hz) reliable_hz="$2"; shift 2 ;;
        --payload-bytes) payload_bytes="$2"; shift 2 ;;
        --bone-count) bone_count="$2"; shift 2 ;;
        --network) network="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ "${quick}" == true ]]; then
    clients=2
    duration=3
    repetitions=1
fi

if [[ "${suite}" == true && -z "${client_levels}" ]]; then
    client_levels="1,10,20"
fi
if [[ -z "${client_levels}" ]]; then
    client_levels="${clients}"
fi
[[ "${client_levels}" =~ ^[0-9]+(,[0-9]+)*$ ]] || { echo "--client-levels 格式应为 1,10,20" >&2; exit 2; }
IFS=',' read -r -a client_matrix <<< "${client_levels}"

for value in "${clients}" "${repetitions}" "${payload_bytes}"; do
    [[ "${value}" =~ ^[0-9]+$ && "${value}" -gt 0 ]] || { echo "整数参数必须 > 0" >&2; exit 2; }
done

# ---------------------------------------------------------------------------
# Phase 6.2: 按平台选择弱网注入方式
#   - 应用层注入（impairment.py）总是随 --network 生效（无需 sudo，可进 CI）。
#   - 内核级整形作为"权威"补充：Linux=netem.sh，macOS=dnctl.sh，都需 sudo，
#     且会改系统网络状态，因此**只提示不自动执行**，由用户显式 sudo 运行。
# ---------------------------------------------------------------------------
case "${network}" in
    unshaped|broadband|mobile|weak) ;;
    *) echo "未知 --network: ${network}（可选 unshaped|broadband|mobile|weak）" >&2; exit 2 ;;
esac
os_name="$(uname -s)"
if [[ "${network}" != "unshaped" ]]; then
    case "${os_name}" in
        Linux)
            echo "[net] Linux：应用层注入已启用（profile=${network}）。"
            echo "[net] 如需内核级权威整形，请另开终端手动运行："
            echo "[net]   sudo benchmark/netem.sh apply <iface> ${network}"
            ;;
        Darwin)
            echo "[net] macOS：应用层注入已启用（profile=${network}，无需 sudo）。"
            if sudo -n true 2>/dev/null; then
                echo "[net] 检测到免密 sudo；如需内核级粗筛（仅 delay+loss）可手动运行："
                echo "[net]   sudo benchmark/dnctl.sh apply lo0 ${network}"
            else
                echo "[net] 无免密 sudo；跳过 dnctl 内核整形，仅用应用层注入。"
            fi
            ;;
        *)
            echo "[net] ${os_name}：应用层注入已启用（profile=${network}）。" ;;
    esac
else
    echo "[net] network=unshaped：不注入任何损伤。"
fi

run_id="$(date +%Y%m%d_%H%M%S)"
result_dir="${script_dir}/results/${run_id}"
report_file="${script_dir}/reports/${run_id}.md"
mkdir -p "${script_dir}/bin" "${result_dir}" "${script_dir}/reports"

cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}" 2>/dev/null || true
        local attempts=0
        while kill -0 "${server_pid}" 2>/dev/null && (( attempts < 30 )); do
            sleep 0.1
            attempts=$((attempts + 1))
        done
        if kill -0 "${server_pid}" 2>/dev/null; then
            kill -9 "${server_pid}" 2>/dev/null || true
        fi
        wait "${server_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

wait_port() {
    local port="$1"
    local attempts=0
    while (( attempts < 100 )); do
        if (echo >/dev/tcp/127.0.0.1/"${port}") 2>/dev/null; then return 0; fi
        sleep 0.1
        attempts=$((attempts + 1))
    done
    return 1
}

start_server() {
    local command="$1"
    local log="$2"
    local port="$3"
    cleanup
    server_pid=""
    (cd "${repo_root}" && exec ${command}) >"${log}" 2>&1 &
    server_pid="$!"
    wait_port "${port}" || { tail -40 "${log}" >&2; echo "服务启动失败" >&2; exit 1; }
}

echo "[0/5] 采集构建前系统资源采样"
python3 "${script_dir}/generate_report.py" --sample "${result_dir}/resource_samples.json" --tag build_before

echo "[1/5] 编译 reference server"
"${CXX:-c++}" -std=c++20 -O3 -pthread "${script_dir}/reference_server.cpp" -o "${reference_bin}"

echo "[2/5] 检查 Ork 服务端分发包与 Python 环境"
os_name_server="$(uname -s)"
arch="$(uname -m)"
case "${os_name_server}-${arch}" in
    Darwin-arm64) platform="macos-arm64" ;;
    Linux-x86_64) platform="linux-x64" ;;
    *) echo "不支持的平台: ${os_name_server}-${arch}（见 server/README.md）" >&2; exit 1 ;;
esac
ork_server_bin="${repo_root}/server/${platform}/bin/embed_minimal"
if [[ ! -x "${ork_server_bin}" ]]; then
    echo "服务端二进制不存在: ${ork_server_bin}" >&2
    echo "请先打包: bash scripts/pack_server.sh（见 server/README.md）" >&2
    exit 1
fi
if [[ ! -x "${python_bin}" ]]; then
    python3 -m venv "${repo_root}/python/.venv"
    "${repo_root}/python/.venv/bin/pip" install -r "${repo_root}/python/requirements.txt"
fi
if [[ "$(uname -s)" == "Darwin" ]]; then
    bridge="${repo_root}/python/lib/kcp/libkcp_bridge.dylib"
else
    bridge="${repo_root}/python/lib/kcp/libkcp_bridge.so"
fi
if [[ ! -f "${bridge}" ]]; then
    (cd "${repo_root}/python" && bash lib/kcp/build.sh)
fi

echo "[2.5/5] 采集构建后与 benchmark 前资源采样"
python3 "${script_dir}/generate_report.py" --sample "${result_dir}/resource_samples.json" --tag build_after
python3 "${script_dir}/generate_report.py" --sample "${result_dir}/resource_samples.json" --tag bench_before

common_args=(--host 127.0.0.1 --duration "${duration}" \
    --pose-hz "${pose_hz}" --reliable-hz "${reliable_hz}" \
    --payload-bytes "${payload_bytes}" --bone-count "${bone_count}" --network "${network}")

echo "[3/5] 运行 reference baselines"
start_server "${reference_bin} --port ${reference_port}" "${result_dir}/reference_server.log" "${reference_port}"
for level in "${client_matrix[@]}"; do
    for repetition in $(seq 1 "${repetitions}"); do
        for transport in tcp_only udp_only tcp_udp; do
            echo "  ${transport} clients=${level} repetition ${repetition}/${repetitions}"
            "${python_bin}" "${script_dir}/benchmark.py" --transport "${transport}" \
                --port "${reference_port}" --clients "${level}" "${common_args[@]}" \
                --output "${result_dir}/${transport}_c${level}_r${repetition}.json"
        done
    done
done

echo "[4/5] 运行真实 Ork Hybrid"
start_server "${ork_server_bin} --gamePort ${ork_port} --workspace ${repo_root}/server" "${result_dir}/ork_server.log" "${ork_port}"
for level in "${client_matrix[@]}"; do
    for repetition in $(seq 1 "${repetitions}"); do
        echo "  ork_hybrid clients=${level} repetition ${repetition}/${repetitions}"
        "${python_bin}" "${script_dir}/benchmark.py" --transport ork_hybrid \
            --port "${ork_port}" --clients "${level}" "${common_args[@]}" \
            --output "${result_dir}/ork_hybrid_c${level}_r${repetition}.json"
    done
done

echo "[4.5/5] 采集 benchmark 后资源采样与可复现元数据"
python3 "${script_dir}/generate_report.py" --sample "${result_dir}/resource_samples.json" --tag bench_after
python3 "${script_dir}/generate_report.py" --emit-meta "${result_dir}/run_metadata.json" \
    --build-dir "${build_dir}" \
    --bench-command "ORK_BUILD_DIR=${build_dir} $(basename "$0")$(if [[ ${#original_args[@]} -gt 0 ]]; then printf ' '; shell_quote "${original_args[@]}"; fi)"

echo "[5/5] 生成 Markdown 报告"
python3 "${script_dir}/generate_report.py" --results "${result_dir}" --output "${report_file}"
# quick 模式（2 clients / 3s / 1 rep，多为 Debug）仅用于冒烟，不得覆盖正式基线 latest.md。
if [[ "${quick}" == true ]]; then
    echo "完成: ${report_file}"
    echo "[quick] 冒烟结果仅写入带时间戳报告，未更新 reports/latest.md（正式基线需用 Release 构建的非 --quick 运行生成）。"
else
    cp "${report_file}" "${script_dir}/reports/latest.md"
    echo "完成: ${report_file}"
    echo "最新报告: ${script_dir}/reports/latest.md"
fi
