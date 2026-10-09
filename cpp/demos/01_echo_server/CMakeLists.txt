# Demo 01 —— 纯 C11 的 C ABI 最小回显服务端（Host）。
# main.c 只 include <ork/sdk/c/ork_abi.h>，证明 C ABI 表面不泄漏任何 C++ 类型；
# 但链接仍须走 C++ 驱动（见顶层 ork_configure_demo 的 LINKER_LANGUAGE 说明）。
add_executable(demo_01_echo_server main.c)
set_target_properties(demo_01_echo_server PROPERTIES OUTPUT_NAME 01_echo_server)
ork_configure_demo(demo_01_echo_server)
