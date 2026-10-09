# Demo 02 —— 纯 C11 的 C ABI 最小客户端（Client）。
# main.c 只 include <ork/sdk/c/ork_client_abi.h>（它自身 include ork_abi.h
# 以复用 OrkError / OrkSendResult / OrkChannel / OrkDisconnectReason）。
add_executable(demo_02_echo_client main.c)
set_target_properties(demo_02_echo_client PROPERTIES OUTPUT_NAME 02_echo_client)
ork_configure_demo(demo_02_echo_client)
