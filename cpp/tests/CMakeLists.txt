# =============================================================================
#  CTest 用例 —— 三个测试全部 **自包含**：不依赖 server/ 下预编译服务端，
#  需要监听端口的用例在运行时向 OS 申请临时端口，因此：
#    * 不会和 9400 上正在跑的 demo 服务端抢端口；
#    * 可以安全地 `ctest -j8` 并行执行。
#
#  日志：默认静音（Ork 内部日志写 /dev/null 且级别 Off）。排障时：
#    ORK_LOG_FILE=/tmp/ork.log ORK_LOG_LEVEL=debug ctest --output-on-failure
# =============================================================================

# ---- C ABI Host smoke：create / start / pump / stats / stop / destroy ------
add_executable(test_c_abi_host test_c_abi_host.c)
ork_configure_demo(test_c_abi_host)

# ---- C ABI Client smoke：create / 初始状态 / 配置校验 / destroy ------------
add_executable(test_c_abi_client test_c_abi_client.c)
ork_configure_demo(test_c_abi_client)

# ---- C++ SDK 往返：进程内 SessionFramework(host) + OrkClient 双通道 echo ---
add_executable(test_cpp_roundtrip test_cpp_roundtrip.cpp)
ork_configure_demo(test_cpp_roundtrip)

add_test(NAME c_abi_host    COMMAND test_c_abi_host)
add_test(NAME c_abi_client  COMMAND test_c_abi_client)
add_test(NAME cpp_roundtrip COMMAND test_cpp_roundtrip)

set_tests_properties(c_abi_host c_abi_client cpp_roundtrip PROPERTIES
    TIMEOUT 120
    LABELS "ork;cpp")
