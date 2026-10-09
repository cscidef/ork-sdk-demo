/**
 * @file bootstrap_security_mode.hpp
 * @brief Bootstrap 通道安全模式（BootstrapSecurityDesign Rev 3 第七章，A1.1）
 *
 * 两种模式：
 * - PlaintextDevelopment：明文 TCP。仅在 CMake 显式
 *   -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 时编入（默认 OFF，编译期剔除）；
 *   编入后服务端 bind 与客户端目标仍强制 loopback，启动输出醒目警告横幅。
 * - TlsServerAuthenticated：TLS 1.3 服务端认证（生产默认）。证书链/私钥
 *   缺失即 fail closed，不存在 TLS 失败后的明文回退。
 *
 * 编译开关不以 Debug/Release 为安全边界；两个常量供运行期校验
 * （Create()/connect 前 fail closed）与测试模式感知使用。
 */
#pragma once

#include <stdexcept>
#include <string>

namespace ork::platform
{

enum class BootstrapSecurityMode
{
    /// 明文 TCP（仅本地开发）。须 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 编入，
    /// 且两端（服务端 bind / 客户端目标）均为 loopback，否则拒绝。
    PlaintextDevelopment = 0,

    /// TLS 1.3 服务端认证（生产默认）。fail closed，无明文回退。
    TlsServerAuthenticated = 1,
};

/// 明文 bootstrap 模式是否编入本构建（CMake ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON）
inline constexpr bool kPlaintextBootstrapCompiledIn =
#if defined(ORK_ALLOW_PLAINTEXT_BOOTSTRAP)
    true;
#else
    false;
#endif

/// TLS 1.3 后端是否编入本构建（非 Windows 且 OpenSSL >= 3；ORK_HAVE_TLS）
inline constexpr bool kTlsBootstrapCompiledIn =
#if defined(ORK_HAVE_TLS)
    true;
#else
    false;
#endif

inline constexpr const char* ToString(BootstrapSecurityMode mode)
{
    switch (mode)
    {
    case BootstrapSecurityMode::PlaintextDevelopment:
        return "PlaintextDevelopment";
    case BootstrapSecurityMode::TlsServerAuthenticated:
        return "TlsServerAuthenticated";
    }
    return "Unknown";
}

/**
 * @brief 解析配置/YAML 字符串（大小写敏感）
 *
 * - "tls"            -> TlsServerAuthenticated
 * - "plaintext_dev"  -> PlaintextDevelopment
 * - 其他值 throw std::runtime_error（含实际值），调用方 fail closed。
 */
inline BootstrapSecurityMode ParseBootstrapSecurityMode(const std::string& value)
{
    if (value == "tls")
        return BootstrapSecurityMode::TlsServerAuthenticated;
    if (value == "plaintext_dev")
        return BootstrapSecurityMode::PlaintextDevelopment;
    throw std::runtime_error(
        "bootstrap_security must be \"tls\" or \"plaintext_dev\" (got \"" + value + "\")");
}

}  // namespace ork::platform
