# Demo 02 —— 纯 C11 的 C ABI 最小客户端（Client）。
# main.c 的 Ork 依赖全部经由 demos/common/demo_support_c.h 引入（F1）：那份头
# 带进 ork_client_abi.h（它自身 include ork_abi.h 以复用 OrkError /
# OrkSendResult / OrkChannel / OrkDisconnectReason），外加计时、25B 探针帧、
# 枚举名、配置填充（F8）与双通道探针循环的函数指针版（F2）。
# 链接仍须走 C++ 驱动，include 路径也由 ork_configure_demo 统一挂上（同 01）。
add_executable(demo_02_echo_client main.c)
set_target_properties(demo_02_echo_client PROPERTIES OUTPUT_NAME 02_echo_client)
ork_configure_demo(demo_02_echo_client)
