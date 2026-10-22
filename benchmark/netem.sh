#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "用法: sudo $0 apply <interface> <broadband|mobile|weak> | clear <interface>" >&2
}

[[ "$(uname -s)" == "Linux" ]] || { echo "netem.sh 仅支持 Linux" >&2; exit 2; }
[[ $# -ge 2 ]] || { usage; exit 2; }

action="$1"
interface="$2"
[[ "${interface}" =~ ^[a-zA-Z0-9_.:-]+$ ]] || { echo "非法网卡名" >&2; exit 2; }

if [[ "${action}" == "clear" ]]; then
    tc qdisc del dev "${interface}" root 2>/dev/null || true
    echo "已清除 ${interface} netem"
    exit 0
fi

[[ "${action}" == "apply" && $# -eq 3 ]] || { usage; exit 2; }
profile="$3"
tc qdisc del dev "${interface}" root 2>/dev/null || true
case "${profile}" in
    broadband) tc qdisc add dev "${interface}" root netem delay 20ms 2ms loss 0.1% ;;
    mobile) tc qdisc add dev "${interface}" root netem delay 60ms 10ms loss 1% reorder 0.2% ;;
    weak) tc qdisc add dev "${interface}" root netem delay 120ms 30ms loss 3% reorder 1% ;;
    *) echo "未知 profile: ${profile}" >&2; exit 2 ;;
esac
echo "已应用 ${profile} 到 ${interface}；运行测试时设置 BENCH_NETWORK=${profile}"

