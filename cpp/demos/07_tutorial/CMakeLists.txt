# Demo 07 —— C++20 SDK 教程：纯 Mock 演示 IHostListener/ISender 回调语义
#            （迁移自 harmonia_ork source/examples/tutorial/sdk_demo.cpp；
#              无网络依赖，PlaintextDevelopment 宏由 Ork::Core 提供）。
add_executable(demo_07_tutorial main.cpp)
set_target_properties(demo_07_tutorial PROPERTIES OUTPUT_NAME 07_tutorial)
ork_configure_demo(demo_07_tutorial)
