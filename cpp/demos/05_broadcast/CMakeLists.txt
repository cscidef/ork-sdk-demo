# Demo 05 —— 纯 C11 的 C ABI 广播与扇出：
#   ork_host_broadcast（全员）+ 宿主自维护连接表后按 connection_id 用
#   ork_host_send 逐个发送（"筛选广播"）。C ABI 没有 Where/Of 谓词集合，
#   也没有 BroadcastReport 回执，所以筛选的计数由宿主自己统计。
add_executable(demo_05_broadcast main.c)
set_target_properties(demo_05_broadcast PROPERTIES OUTPUT_NAME 05_broadcast)
ork_configure_demo(demo_05_broadcast)
