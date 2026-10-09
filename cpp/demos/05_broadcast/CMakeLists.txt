# Demo 05 —— C++20 广播：ISender::Broadcast + ConnectionSet(All/Of/Where)
#            + BroadcastReport 一致性校验。
add_executable(demo_05_broadcast main.cpp)
set_target_properties(demo_05_broadcast PROPERTIES OUTPUT_NAME 05_broadcast)
ork_configure_demo(demo_05_broadcast)
