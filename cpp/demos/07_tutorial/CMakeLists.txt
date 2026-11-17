# Demo 07 —— 纯 C11 的 C ABI 回调与生命周期教程：
#            同进程内起一条**真实** Host + Client（真的走 TCP bootstrap →
#            UDP 握手 → KCP），分步展示 on_connected → 应用层命令(PING/ECHO)
#            → 宿主 ork_host_disconnect 主动踢人 → on_disconnected。
#            不是 Mock：C ABI 无法替换 OrkHost 实现，模拟只会教出错误的
#            线程与生命周期预期。
add_executable(demo_07_tutorial main.c)
set_target_properties(demo_07_tutorial PROPERTIES OUTPUT_NAME 07_tutorial)
ork_configure_demo(demo_07_tutorial)
