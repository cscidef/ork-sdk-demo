#pragma once
/**
 * @file demo_support.hpp
 * @brief C++ demo / test 共用的小工具
 *
 * 这些代码 **不是** Ork SDK 的一部分，只是为了让 03~06 号 demo 与
 * tests/test_cpp_roundtrip.cpp 保持短小可读而抽出来的公共片段：
 *
 *   - InitLogging()      初始化 Ork 内部日志（否则 pre-init 日志会淹没 stdout）
 *   - PickFreePort()     向 OS 申请一个 TCP + UDP 都空闲的临时端口
 *   - BuildEchoFrame()   组装 25B "ORKBENCH1" opaque 探针帧
 *   - MakeHostConfig()   TransportConfig 模板（C++ SDK 服务端）
 *   - MakeClientConfig() ork::client::ClientConfig 模板（C++ SDK 客户端）
 *   - InstallSignalHandlers()  Ctrl+C 干净退出
 *
 * 纯 C 的 demo（01 / 02）不使用本头文件 —— 它们只依赖 C ABI 头。
 */

#include "ork/client/client_config.hpp"
#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/platform/log/log_level.hpp"
#include "ork/platform/log/logger.hpp"
#include "ork/sdk/sdk/session_interface.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace orkdemo {

// ============================================================================
// 25B opaque 回显帧
//
//   magic(9B "ORKBENCH1") + marker(8B) + 单调时间戳 ns(8B, 小端) = 25B
//
// 这是 Ork 各语言 demo 通用的探针格式：预编译服务端 embed_minimal 的
// TransportDemoHandler 对「长度恰为 25 且前缀 ORKBENCH1」的 payload 走
// opaque 原样回显路径，不尝试 protobuf 解析。
// ============================================================================
inline constexpr std::size_t      kEchoFrameSize      = 25;
inline constexpr std::string_view kEchoMagic          = "ORKBENCH1";
inline constexpr std::string_view kMarkerReliable     = "ORKDEMO1";
inline constexpr std::string_view kMarkerUnreliable   = "ORKDEMO2";

#if defined(_WIN32)
inline constexpr const char* kDevNull = "NUL";
#else
inline constexpr const char* kDevNull = "/dev/null";
#endif

// ============================================================================
// 时间
// ============================================================================
inline uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline double NowMs() noexcept { return static_cast<double>(NowNs()) / 1.0e6; }

inline void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline void PutU64Le(uint8_t* p, uint64_t v) noexcept
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

inline uint64_t GetU64Le(const uint8_t* p) noexcept
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

/**
 * @brief 组装一个 25B ORKBENCH1 探针帧
 * @param out    目标缓冲，size() 必须 == kEchoFrameSize
 * @param marker 8 字节标记（如 kMarkerReliable）
 */
inline void BuildEchoFrame(std::span<uint8_t> out, std::string_view marker)
{
    if (out.size() != kEchoFrameSize || marker.size() != 8)
    {
        std::fprintf(stderr, "[demo_support] BuildEchoFrame: 参数非法\n");
        std::abort();
    }
    std::memcpy(out.data(), kEchoMagic.data(), kEchoMagic.size());
    std::memcpy(out.data() + kEchoMagic.size(), marker.data(), 8);
    PutU64Le(out.data() + kEchoMagic.size() + 8, NowNs());
}

inline bool IsEchoMagic(std::span<const uint8_t> payload) noexcept
{
    return payload.size() >= kEchoMagic.size()
        && std::memcmp(payload.data(), kEchoMagic.data(), kEchoMagic.size()) == 0;
}

// ============================================================================
// 日志
// ============================================================================
inline const char* EnvOr(const char* name, const char* fallback) noexcept
{
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : fallback;
}

inline LogLevel ParseLogLevel(std::string_view v, LogLevel fallback) noexcept
{
    if (v == "trace") return LogLevel::Trace;
    if (v == "debug") return LogLevel::Debug;
    if (v == "info")  return LogLevel::Info;
    if (v == "warn")  return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    if (v == "off")   return LogLevel::Off;
    return fallback;
}

/**
 * @brief 初始化 Ork 内部日志
 *
 * **必须调用**：Logger 未 Init 时处于 pre-init 降级状态，所有级别的内部日志
 * 会不加过滤地直写 stderr（还带 harmonia_ork 的源码绝对路径），demo 输出会被
 * 完全淹没。Init 之后级别过滤生效，同时挂 stdout 彩色 sink + 文件 sink。
 *
 * 环境变量：
 *   ORK_LOG_FILE   日志文件路径（默认 /dev/null，即丢弃）
 *   ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 defaultLevel）
 */
inline void InitLogging(const char* appName, LogLevel defaultLevel = LogLevel::Warn)
{
    const char* file = EnvOr("ORK_LOG_FILE", kDevNull);
    const LogLevel lv = ParseLogLevel(EnvOr("ORK_LOG_LEVEL", ""), defaultLevel);
    try
    {
        Logger::Instance().Init(appName, file, lv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[demo_support] Logger::Init 失败（忽略）: %s\n", e.what());
    }
}

// ============================================================================
// 临时端口
// ============================================================================
/**
 * @brief 向 OS 申请一个 TCP 与 UDP 都空闲的 loopback 端口
 *
 * Ork 的 TCP bootstrap 与 UDP/KCP **共用同一个端口**，所以必须两边都探。
 * 返回后 socket 立即析构释放端口，理论上存在被抢占的竞态窗口，
 * 对单机 demo / ctest 足够。返回 0 表示 64 次尝试都失败。
 */
inline uint16_t PickFreePort()
{
    asio::io_context io;
    const auto loopback = asio::ip::make_address_v4("127.0.0.1");

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        asio::error_code ec;

        asio::ip::udp::socket udp(io);
        udp.open(asio::ip::udp::v4(), ec);
        if (ec) continue;
        udp.bind(asio::ip::udp::endpoint(loopback, 0), ec);
        if (ec) continue;
        const uint16_t port = udp.local_endpoint().port();

        asio::ip::tcp::acceptor tcp(io);
        tcp.open(asio::ip::tcp::v4(), ec);
        if (ec) continue;
        tcp.bind(asio::ip::tcp::endpoint(loopback, port), ec);
        if (ec) continue;

        return port;   // udp / tcp 两个 socket 随作用域析构，端口被释放
    }
    return 0;
}

// ============================================================================
// 配置模板
// ============================================================================
/**
 * @brief C++ SDK 服务端配置模板
 *
 * 与 server/config/network.yaml 对齐：KCP on、100Hz KCP / 30Hz pump、
 * max_packet_size 1200、空闲超时 300s、AEAD 加密 on。
 *
 * bootstrapSecurity 固定为 PlaintextDevelopment —— 明文 bootstrap 只在
 * `-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON` 构建里编入，且服务端 bind 会被强制
 * 收敛到 loopback。生产环境请改用 TlsServerAuthenticated 并提供
 * tlsCertificateFile / tlsPrivateKeyFile。
 */
inline TransportConfig MakeHostConfig(uint16_t port, bool encryption = true)
{
    TransportConfig cfg;
    cfg.gamePort                 = port;
    cfg.enableKcp                = true;
    cfg.kcpUpdateRate            = 100;   // 必须 >= pumpRate
    cfg.pumpRate                 = 30;
    cfg.pumpMaxFrameTimeMs       = 0;     // 0 = 按 pumpRate 推算
    cfg.enableEncryption         = encryption;
    cfg.bootstrapTtlSec          = 300;
    cfg.cipherDebugLog           = false;
    cfg.connectionIdleTimeoutSec = 300;
    cfg.maxPacketSize            = 1200;
    cfg.bootstrapSecurity =
        ork::platform::BootstrapSecurityMode::PlaintextDevelopment;
    // kcpConfig / connectionProtection 用默认值即可
    //（默认值与 server/config/network.yaml 完全一致）
    return cfg;
}

/**
 * @brief C++ SDK 客户端配置模板
 *
 * ★ tcpPort 与 udpPort 必须相同：demo 服务端 TCP bootstrap 与 UDP/KCP 共用
 *   一个端口，而 ClientConfig 的 udpPort 默认值是 9401，留默认必然连不上。
 */
inline ork::client::ClientConfig MakeClientConfig(const std::string& host,
                                                  uint16_t port,
                                                  bool encryption = true)
{
    ork::client::ClientConfig cfg;
    cfg.host              = host;
    cfg.tcpPort           = port;
    cfg.udpPort           = port;      // ★ 见上面的说明
    cfg.enableKcp         = true;
    cfg.enableEncryption  = encryption;   // 意向；权威是服务端 BootstrapFrame.flags
    cfg.maxPacketSize     = 1200;
    cfg.connectTimeoutMs  = 5000;
    cfg.udpHandshakeRetryIntervalMs = 500;
    cfg.securityMode =
        ork::platform::BootstrapSecurityMode::PlaintextDevelopment;
    return cfg;
}

// ============================================================================
// 信号
// ============================================================================
namespace detail {
inline std::atomic<bool>* g_stopFlag = nullptr;
inline void SignalTrampoline(int) noexcept
{
    if (g_stopFlag != nullptr) g_stopFlag->store(true, std::memory_order_relaxed);
}
}  // namespace detail

/**
 * @brief 安装 SIGINT / SIGTERM 处理器
 * @param stopFlag 调用方持有的原子标志；主循环轮询它即可干净退出
 *
 * 处理器只置位，不做任何真实清理 —— 清理必须回到主线程，因为
 * SessionFramework::Stop() 会 join 内部 IO 线程，在信号上下文里调用不安全。
 */
inline void InstallSignalHandlers(std::atomic<bool>& stopFlag) noexcept
{
    detail::g_stopFlag = &stopFlag;
    std::signal(SIGINT,  &detail::SignalTrampoline);
    std::signal(SIGTERM, &detail::SignalTrampoline);
}

}  // namespace orkdemo
