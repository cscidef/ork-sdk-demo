#!/usr/bin/env bash
# =============================================================================
# macOS dnctl + pfctl 网络损伤注入（Phase 6.2 Part B）
#
# 与 netem.sh 接口完全一致：
#   sudo dnctl.sh apply <interface> <broadband|mobile|weak>
#   sudo dnctl.sh clear <interface>
#
# ⚠️ 需要 sudo，会修改系统网络状态（dummynet pipe + pf anchor）。
# ⚠️ dnctl 只支持 delay + plr（丢包率），没有 jitter / reorder / dup，
#    因此本脚本只能做"粗筛"；要完整对齐 netem 的抖动/乱序，请用应用层
#    注入（python/client/impairment.py，benchmark.py --network 自动启用）。
#
# 关于回环：benchmark 默认打 127.0.0.1，流量走 lo0。要整形回环流量，
#   interface 传 lo0；要整形真实网卡（如跨机测试），传 en0 / en1 等。
#
# 本脚本只负责"写规则"，不会自动被 run_benchmark.sh 执行（避免误改系统
# 状态）；需要内核级整形时由用户显式 sudo 运行。
# =============================================================================
set -euo pipefail

usage() {
    echo "用法: sudo $0 apply <interface> <broadband|mobile|weak> | clear <interface>" >&2
}

[[ "$(uname -s)" == "Darwin" ]] || { echo "dnctl.sh 仅支持 macOS（Linux 请用 netem.sh）" >&2; exit 2; }
[[ $# -ge 2 ]] || { usage; exit 2; }

action="$1"
interface="$2"
[[ "${interface}" =~ ^[a-zA-Z0-9_.:-]+$ ]] || { echo "非法网卡名: ${interface}" >&2; exit 2; }

# dnctl / pfctl 都需要 root
if [[ "$(id -u)" -ne 0 ]]; then
    echo "错误：dnctl/pfctl 需要 root 权限，请用 sudo 运行" >&2
    exit 2
fi

# pf anchor 名（点号是 anchor 层级分隔符，用 ork.bench 挂在独立子树，
# flush 时只清自己的规则，不碰系统主 pf 配置）
PF_ANCHOR="ork.bench"
PF_CONF="$(mktemp -t ork_bench_pf).conf"
# 出向用 pipe 1，入向用 pipe 2（双向都整形，模拟真实链路）
PIPE_OUT=1
PIPE_IN=2

cleanup_conf() { rm -f "${PF_CONF}"; }
trap cleanup_conf EXIT

clear_all() {
    # 先卸 pf 规则，再删 pipe；-q 静默、|| true 容忍"本来就没有"
    pfctl -a "${PF_ANCHOR}" -F all 2>/dev/null || true
    dnctl -q pipe "${PIPE_OUT}" delete 2>/dev/null || true
    dnctl -q pipe "${PIPE_IN}" delete 2>/dev/null || true
    echo "已清除 ${interface} 的 dnctl pipe(${PIPE_OUT},${PIPE_IN}) 与 pf anchor '${PF_ANCHOR}'"
}

if [[ "${action}" == "clear" ]]; then
    clear_all
    exit 0
fi

[[ "${action}" == "apply" && $# -eq 3 ]] || { usage; exit 2; }
profile="$3"

# 三档参数（与 netem.sh / impairment.py 的 delay、loss 对齐）
# dnctl 的 plr 是 0..1 的丢包概率；netem 的 loss 是百分比。
case "${profile}" in
    broadband) delay_ms=20;  plr=0.001 ;;   # netem: delay 20ms  loss 0.1%
    mobile)    delay_ms=60;  plr=0.01  ;;   # netem: delay 60ms  loss 1%
    weak)      delay_ms=120; plr=0.03  ;;   # netem: delay 120ms loss 3%
    *) echo "未知 profile: ${profile}" >&2; usage; exit 2 ;;
esac

# 幂等：先清掉旧规则再重建，避免重复 apply 叠加
clear_all

# 配置 dummynet pipe：delay 单位 ms，plr 0..1，queue 限制排队槽数防膨胀
dnctl pipe "${PIPE_OUT}" config delay "${delay_ms}" plr "${plr}" queue 50
dnctl pipe "${PIPE_IN}"  config delay "${delay_ms}" plr "${plr}" queue 50

# 写 pf 规则：把指定网卡上的 TCP/UDP 流量分别导入出/入 pipe。
# dummynet 规则必须放在 anchor 里，且 anchor 要被主 pf.conf 以
# `dummynet-anchor "ork.bench"` 引用才会生效；多数 macOS 默认 pf.conf
# 不含该行，因此这里同时尝试 anchor 挂载 + 直接 -f 加载两种写法。
cat >"${PF_CONF}" <<EOF
dummynet out on ${interface} proto { tcp, udp } pipe ${PIPE_OUT}
dummynet in on ${interface} proto { tcp, udp } pipe ${PIPE_IN}
EOF

pfctl -a "${PF_ANCHOR}" -f "${PF_CONF}"
# 确保 pf 处于启用状态（-e 幂等；已启用时返回非 0 也不致命）
pfctl -e 2>/dev/null || true

echo "已应用 ${profile} 到 ${interface}（delay=${delay_ms}ms plr=${plr}，双向 pipe ${PIPE_OUT}/${PIPE_IN}）"
echo "运行测试时设置 BENCH_NETWORK=${profile}，让报告 network 字段如实标注"
echo "清除：sudo $0 clear ${interface}"
echo ""
echo "提示：若 pf anchor 未被主 pf.conf 引用，规则可能不生效；可用"
echo "      'sudo pfctl -a ${PF_ANCHOR} -s rules' 与 'sudo dnctl list' 核对。"
echo "      需要 jitter/reorder/dup 时请改用应用层注入（benchmark.py --network ${profile}）。"
