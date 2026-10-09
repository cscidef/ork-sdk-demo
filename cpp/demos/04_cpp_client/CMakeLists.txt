# Demo 04 —— C++20 SDK 客户端：OrkClient + IClientListener，手动 PumpOnce 驱动。
add_executable(demo_04_cpp_client main.cpp)
set_target_properties(demo_04_cpp_client PROPERTIES OUTPUT_NAME 04_cpp_client)
ork_configure_demo(demo_04_cpp_client)
