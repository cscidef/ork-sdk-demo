# Demo 01 —— 纯 C11 的 C ABI 最小回显服务端（Host）。
# main.c 的 Ork 依赖全部经由 demos/common/demo_support_c.h 引入（F1）：那份头
# 只含 C 标准头 + ork_abi.h + ork_client_abi.h + 平台差异宏 + static inline 小
# 工具，所以「C ABI 表面不泄漏任何 C++ 类型」这条结论仍然成立。但链接仍须走
# C++ 驱动，include 路径也由 ork_configure_demo 统一挂上（LINKER_LANGUAGE 的
# 理由见顶层 CMakeLists）。
add_executable(demo_01_echo_server main.c)
set_target_properties(demo_01_echo_server PROPERTIES OUTPUT_NAME 01_echo_server)
ork_configure_demo(demo_01_echo_server)
