# Demo 06 —— C ABI 加密通道：进程内起一个 enable_encryption=1 的 Host，
#            再用 C ABI Client 连上去，通过 OrkConnectionInfo.encryption_active
#            与 OrkStats.crypto_fail_* 证明 AEAD 密钥在 bootstrap 阶段协商成功、
#            且业务层收发是完全透明的（明文进 / 明文出）。
add_executable(demo_06_encryption main.cpp)
set_target_properties(demo_06_encryption PROPERTIES OUTPUT_NAME 06_encryption)
ork_configure_demo(demo_06_encryption)
