/**
 * @file tls_context.hpp
 * @brief Bootstrap Security A1.1：TLS 1.3 ssl::context 薄工厂（服务端/客户端）
 *
 * 设计约束（BootstrapSecurityDesign Rev 3 第七/九/十二章）：
 * - 薄抽象：不做 ITlsBackend 纯虚接口（难以桥接 Asio handler），直接产出
 *   asio::ssl::context（shared_ptr，生命周期由 TcpTransportServer / OrkClient
 *   持有并覆盖所有 stream）。
 * - TLS min/max 固定 1.3（SSL_CTX_set_{min,max}_proto_version = TLS1_3_VERSION）。
 * - 0-RTT early data 固定禁用（SSL_CTX_set_max_early_data = 0），无任何
 *   "allow early data" 可配置开关；1-RTT session resumption 允许。
 * - 客户端 verify_peer + SNI + SAN/hostname 验证（X509_VERIFY_PARAM_set1_host，
 *   per-SSL 设置见 ConfigureClientSslForHostname）。
 * - 服务端私钥加载前 POSIX owner-only 权限检查（stat：普通文件 + 属主为当前
 *   用户 + 无 group/other 位），不合规 fail closed。
 * - 任何失败 throw std::runtime_error（fail closed），调用方不得回退明文。
 *
 * Windows 本轮不实现：ORK_HAVE_TLS 未定义时本头文件内容整体剔除。
 */
#pragma once

#include "asio.hpp"

#if defined(ORK_HAVE_TLS)

#include <asio/ssl.hpp>
#include <memory>
#include <string>

namespace ork::transport
{

/** @brief 服务端 TLS 参数（TLS 模式必填项由 NetworkConfig/TransportConfig 校验保证非空） */
struct TlsServerParams
{
    std::string certificate_file;     ///< PEM 证书链（leaf 在前，含中间链）
    std::string private_key_file;     ///< PEM 私钥（加载前 POSIX 权限检查）
    int handshake_timeout_ms = 5000;  ///< 握手超时（由 accept 路径消费，非 ctx 属性）
};

/**
 * @brief 客户端 TLS 参数
 *
 * 信任锚规则（fail closed）：
 * - ca_file 非空：作为受信 CA 加载（SSL_CTX_load_verify_locations）
 * - use_system_trust_store=true：附加系统默认信任路径
 *   （SSL_CTX_set_default_verify_paths）
 * - 两者皆空/关：无任何信任锚，直接拒绝（不允许"碰运气"验证）
 * expected_server_name 必填非空：SNI + SAN/hostname 验证共用。
 */
struct TlsClientParams
{
    std::string ca_file;
    std::string expected_server_name;
    bool use_system_trust_store = true;
    int handshake_timeout_ms = 5000;
};

/** @brief 创建服务端 ssl::context（tls_server，TLS1.3 only，0-RTT 禁用）。失败 throw。 */
std::shared_ptr<asio::ssl::context> MakeServerTlsContext(const TlsServerParams& params);

/** @brief 创建客户端 ssl::context（tls_client，TLS1.3 only，verify_peer）。失败 throw。 */
std::shared_ptr<asio::ssl::context> MakeClientTlsContext(const TlsClientParams& params);

/**
 * @brief 在客户端 stream 的 SSL 对象上设置 SNI + hostname 验证（握手前调用）
 *
 * - SSL_set_tlsext_host_name：SNI（服务端按名选证书）
 * - X509_VERIFY_PARAM_set1_host：SAN/hostname 验证（证书含 SAN 时只验 SAN；
 *   仅当无 SAN 才回退验 subject CN，符合 RFC 6125）
 */
void ConfigureClientSslForHostname(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                                   const std::string& expected_server_name);

/**
 * @brief POSIX 私钥权限检查（owner-only），加载私钥前调用
 *
 * 要求（任一不满足 throw，fail closed）：
 * - stat 成功（文件存在、可访问；跟随符号链接检查最终目标）
 * - 普通文件（S_ISREG，排除目录/设备/fifo 等）
 * - 属主为当前进程用户（st_uid == geteuid()）
 * - 权限位不含任何 group/other 位（0600/0400 均合法）
 */
void ValidatePrivateKeyPermissions(const std::string& path);

// ===== 诊断/测试访问器（验证 TLS1.3 only 与 0-RTT 禁用的编译产物属性） =====
// 注：native_handle() 非 const 成员，故此处取非 const 引用（仅诊断用途）。

/** @brief SSL_CTX_get_max_early_data（期望恒为 0） */
long GetTlsMaxEarlyData(asio::ssl::context& ctx);

/** @brief SSL_CTX_get_min_proto_version（期望恒为 TLS1_3_VERSION） */
int GetTlsMinProtoVersion(asio::ssl::context& ctx);

/** @brief SSL_CTX_get_max_proto_version（期望恒为 TLS1_3_VERSION） */
int GetTlsMaxProtoVersion(asio::ssl::context& ctx);

}  // namespace ork::transport

#endif  // ORK_HAVE_TLS
