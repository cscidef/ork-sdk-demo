# =============================================================================
#  五个 demo，每个都是独立可执行文件 + 独立 README + 独立 CMakeLists。
#
#  全部 **纯 C11**，只 include C ABI 头（ork_abi.h / ork_client_abi.h）：
#   01  —— 最小回显服务端（Host）：create / start / 回调 / 回显
#   02  —— 最小客户端（Client）：create / connect / 手动 pump / 双通道探针
#   05  —— 广播与扇出：全员 broadcast + 宿主按 connection_id 筛选逐个 send
#   06  —— 加密：AEAD 密钥协商、encryption_active 与透明收发（A/B 对照）
#   07  —— 回调与生命周期：on_connected → 应用层命令 → on_disconnected
#
#  可执行文件统一输出到 build/bin/<name>（名字去掉 demo_ 前缀，见各子目录）。
# =============================================================================
add_subdirectory(01_echo_server)
add_subdirectory(02_echo_client)
add_subdirectory(05_broadcast)
add_subdirectory(06_encryption)
add_subdirectory(07_tutorial)
