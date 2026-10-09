# =============================================================================
#  六个 demo，每个都是独立可执行文件 + 独立 README + 独立 CMakeLists。
#
#  01 / 02 —— 纯 C11，只 include C ABI 头，展示最低层接入面（Unity / FFI 友好）
#  03 / 04 —— C++20 SDK，展示 SessionFramework / OrkClient 的面向对象接入面
#  05      —— C++20，ISender::Broadcast + ConnectionSet + BroadcastReport
#  06      —— C ABI，进程内 host+client 配对，展示 AEAD 加密协商与透明收发
#
#  可执行文件统一输出到 build/bin/<name>（名字去掉 demo_ 前缀，见各子目录）。
# =============================================================================
add_subdirectory(01_echo_server)
add_subdirectory(02_echo_client)
add_subdirectory(03_cpp_host)
add_subdirectory(04_cpp_client)
add_subdirectory(05_broadcast)
add_subdirectory(06_encryption)
