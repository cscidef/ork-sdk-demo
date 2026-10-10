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
 *   - EchoListener       服务端回显监听器（03 / test_cpp_roundtrip 共用）
 *   - EchoProbeListener  客户端探针监听器（04 / test_cpp_roundtrip 共用）
 *   - RunDualChannelProbe()  Reliable + Unreliable 双通道 pump 循环
 *   - InstallSignalHandlers()  Ctrl+C 干净退出
 *
 * 分层：与语言无关的那部分（计时、字节序、探针帧协议常量、枚举名、
 * 配置填充的不变式、临时端口、探针循环节奏）只有一份权威定义 —— 纯 C11 的
 * `demo_support_c.h`。本头文件是它的 C++ 薄封装，另外补上只有 C++ 才
 * 有的东西（std::span 接口、TransportConfig / ClientConfig 映射、
 * 两个监听器类、信号处理）。
 *
 * 纯 C 的 demo（01 / 02）与 tests 下的 C 测试直接 include demo_support_c.h。
 */

#include "demo_support_c.h"

#include "ork/client/client_config.hpp"
#include "ork/client/ork_client.hpp"      // IClientListener（EchoProbeListener 的基类）
#include "ork/platform/data/bootstrap_security_mode.hpp"
#include "ork/platform/log/log_level.hpp"
#include "ork/platform/log/logger.hpp"
#include "ork/sdk/host_listener.hpp"      // IHostListener（EchoListener 的基类）
#include "ork/sdk/sender.hpp"             // ISender
#include "ork/sdk/sdk/session_interface.hpp"

#include <asio.hpp>

#include <array>
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
// 协议常量：值来自 demo_support_c.h，这里只是给 C++ 一个带类型的名字。
inline constexpr std::size_t      kEchoFrameSize      = ORK_ECHO_FRAME_SIZE;
inline constexpr std::string_view kEchoMagic          = ORK_ECHO_MAGIC;
inline constexpr std::string_view kMarkerReliable     = ORK_MARKER_RELIABLE;
inline constexpr std::string_view kMarkerUnreliable   = ORK_MARKER_UNRELIABLE;
inline constexpr std::size_t      kMarkerLen          = ORK_MARKER_LEN;

inline constexpr int kUdpMaxAttempts = ORK_UDP_MAX_ATTEMPTS;
inline constexpr int kUdpRetryMs     = ORK_UDP_RETRY_MS;
inline constexpr int kPumpSleepMs    = ORK_PUMP_SLEEP_MS;

inline constexpr const char* kDevNull = ORK_DEVNULL;

// ============================================================================
// 时间
// ============================================================================
inline uint64_t NowNs() noexcept { return ork_demo_now_ns(); }

inline double NowMs() noexcept { return static_cast<double>(ork_demo_now_ms()); }

inline void SleepMs(int ms) { ork_demo_sleep_ms(ms); }

inline void PutU64Le(uint8_t* p, uint64_t v) noexcept { ork_demo_put_u64_le(p, v); }

inline uint64_t GetU64Le(const uint8_t* p) noexcept { return ork_demo_get_u64_le(p); }

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
    char m[ORK_MARKER_LEN + 1];
    std::memcpy(m, marker.data(), ORK_MARKER_LEN);
    m[ORK_MARKER_LEN] = '\0';
    ork_demo_build_frame(out.data(), m);
}

inline bool IsEchoMagic(std::span<const uint8_t> payload) noexcept
{
    return ork_demo_is_echo_magic(payload.data(),
                                  static_cast<uint32_t>(payload.size())) != 0;
}

// ============================================================================
// 日志
// ============================================================================
inline const char* EnvOr(const char* name, const char* fallback) noexcept
{
    return ork_demo_env_or(name, fallback);
}

/**
 * @brief ORK_LOG_LEVEL → C ABI 的 uint32_t 日志级别
 * @param who 诊断输出用的前缀（如 "host" / "client"）
 */
inline uint32_t EnvLogLevel(const char* who) noexcept
{
    return ork_demo_env_log_level(who);
}

// ============================================================================
// 横幅（F19）
// ============================================================================
inline void PrintBanner(const char* title) { ork_demo_print_banner(title); }

inline void PrintBanner(const std::string& title) { ork_demo_print_banner(title.c_str()); }

inline void PrintBanner(std::string_view title)
{
    ork_demo_print_banner(std::string(title).c_str());
}

/**
 * @brief 只打一条分隔线
 *
 * 多行横幅（demo 01/03/04/05/06 的开头都带 target / port / 版本等若干行）
 * 用不上 PrintBanner，但同样不该再手写 61 个 '=' 字面量 —— 上下各调一次这个。
 */
inline void PrintBannerRule() { std::printf(ORK_BANNER_RULE "\n"); }

/** @brief 小节分隔线（比横幅轻一级，用于结论块） */
inline void PrintSeparator()
{
    std::printf("-------------------------------------------------------------\n");
}

// ============================================================================
// 枚举名 / 统计 —— C ABI 侧
// ============================================================================
inline const char* SendResultName(OrkSendResult r) noexcept
{
    return ork_demo_send_result_name(r);
}

inline const char* DisconnectReasonName(OrkDisconnectReason r) noexcept
{
    return ork_demo_disconnect_reason_name(r);
}

inline const char* ClientStateName(OrkClientState s) noexcept
{
    return ork_demo_client_state_name(s);
}

inline const char* LinkName(OrkLinkType t) noexcept { return ork_demo_link_name(t); }

inline const char* LinkName(uint32_t t) noexcept
{
    return ork_demo_link_name(static_cast<OrkLinkType>(t));
}

/** @brief crypto_fail_* 四项合计（C ABI 的 OrkStats） */
inline unsigned long long CryptoFailTotal(const OrkStats& s) noexcept
{
    return ork_demo_crypto_fail_total(&s);
}

// ============================================================================
// 枚举名 / 统计 —— C++ SDK 侧
//
// ork::sdk::OrkStats 是独立于 C ABI OrkStats 的 C++ 结构体（camelCase），
// 两者没有转换关系，所以这里必须单独实现一份。
// ============================================================================
inline const char* ChannelName(ork::sdk::Channel ch) noexcept
{
    return ch == ork::sdk::Channel::Reliable ? "Reliable" : "Unreliable";
}

inline unsigned long long CryptoFailTotal(const ork::sdk::OrkStats& s) noexcept
{
    return static_cast<unsigned long long>(s.cryptoFailAead)
         + static_cast<unsigned long long>(s.cryptoFailReplay)
         + static_cast<unsigned long long>(s.cryptoFailExpired)
         + static_cast<unsigned long long>(s.cryptoFailMalformed);
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
 * 返回后 socket 立即关闭释放端口，理论上存在被抢占的竞态窗口，
 * 对单机 demo / ctest 足够。返回 0 表示 64 次尝试都失败。
 */
inline uint16_t PickFreePort() { return ork_demo_pick_free_port(); }

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
    // 默认值与全部不变式（tcp_port == udp_port、drive_mode == 0、
    // kcp_update_rate_hz >= pump_rate_hz …）由 C 侧统一给出，
    // 这里只做 C ABI 结构体 → C++ TransportConfig 的字段映射。
    OrkHostConfig c{};
    ork_demo_fill_host_config(&c, port, encryption ? 1 : 0);

    TransportConfig cfg;
    cfg.gamePort                 = c.udp_port;   // TCP bootstrap 与 UDP/KCP 共用
    cfg.enableKcp                = c.enable_kcp != 0;
    cfg.kcpUpdateRate            = c.kcp_update_rate_hz;
    cfg.pumpRate                 = c.pump_rate_hz;
    cfg.pumpMaxFrameTimeMs       = 0;            // 0 = 按 pumpRate 推算
    cfg.enableEncryption         = c.enable_encryption != 0;
    cfg.bootstrapTtlSec          = c.bootstrap_ttl_sec;
    cfg.cipherDebugLog           = false;
    cfg.connectionIdleTimeoutSec = c.idle_timeout_sec;
    cfg.maxPacketSize            = static_cast<int>(c.max_packet_size);
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
                                                  bool encryption = true,
                                                  int connectTimeoutMs = 5000)
{
    // 同上：不变式（udp_port == tcp_port、kcp_stream_mode == 0、
    // struct_size == v2）由 C 侧的 ork_demo_fill_client_config 统一保证。
    OrkClientConfig c{};
    ork_demo_fill_client_config(&c, port, encryption ? 1 : 0, connectTimeoutMs);

    ork::client::ClientConfig cfg;
    cfg.host              = host;                  // C ABI 侧固定 "127.0.0.1"
    cfg.tcpPort           = c.tcp_port;
    cfg.udpPort           = c.udp_port;            // ★ 见上面的说明
    cfg.enableKcp         = c.enable_kcp != 0;
    cfg.enableEncryption  = c.enable_encryption != 0;  // 意向；权威是服务端 BootstrapFrame.flags
    cfg.maxPacketSize     = c.max_packet_size;
    cfg.connectTimeoutMs  = c.connect_timeout_ms;
    // C ABI 没有对应字段（内部固定重试节奏），C++ 侧显式给出。
    cfg.udpHandshakeRetryIntervalMs = 500;
    cfg.securityMode =
        ork::platform::BootstrapSecurityMode::PlaintextDevelopment;
    // tls_use_system_trust_store：0 = 默认(启用) / 1 = 启用 / -1 = 禁用
    cfg.useSystemTrustStore = (c.tls_use_system_trust_store != -1);
    cfg.tlsHandshakeTimeoutMs = c.tls_handshake_timeout_ms;
    // kcpConfig 用默认值（与 C ABI 的 kcp_* 全 0 → 内部默认等价）
    return cfg;
}

// ============================================================================
// 服务端回显监听器（F10）
//
// demo 03 要打印业务日志，test_cpp_roundtrip 要保持 ctest 输出干净 ——
// 同一个类用 verbose 开关区分，统计口径完全一致，避免两份实现漂移。
//
// 线程契约：四个回调都在 core 内部 NetworkPump 的 asio strand 线程上触发，
// 实现必须非阻塞；ISender::Send / IConnectionRegistry::Disconnect 在回调里
// 安全（只入队 / 异步 close），SessionFramework::Stop() 绝对不行（自死锁）。
// 因此所有计数都用 atomic，OnStats 里只缓存标量。
// ============================================================================
class EchoListener final : public ork::sdk::IHostListener
{
public:
    explicit EchoListener(ork::sdk::ISender& sender, bool verbose = true) noexcept
        : sender_(sender), verbose_(verbose)
    {
    }

    void OnConnected(ConnectionId connectionId) override
    {
        lastConnectionId_.store(connectionId, std::memory_order_relaxed);
        const unsigned long long total =
            static_cast<unsigned long long>(
                connections_.fetch_add(1, std::memory_order_relaxed) + 1);
        if (verbose_)
        {
            std::printf("[host] + connection %u  (累计接入 %llu)\n", connectionId, total);
            std::fflush(stdout);
        }
    }

    void OnDisconnected(ConnectionId connectionId, DisconnectReason reason) override
    {
        disconnections_.fetch_add(1, std::memory_order_relaxed);
        if (verbose_)
        {
            std::printf("[host] - connection %u  reason=%s\n", connectionId, ToString(reason));
            std::fflush(stdout);
        }
    }

    void OnPayload(ConnectionId connectionId,
                   ork::sdk::Channel channel,
                   std::span<const uint8_t> payload) override
    {
        payloads_.fetch_add(1, std::memory_order_relaxed);

        // 回显：原样、同通道。
        // ISender::Send 内部只把 payload 拷进出站队列，返回后 span 即可失效。
        const ork::sdk::SendResult r = sender_.Send(connectionId, payload, channel);
        if (r != ork::sdk::SendResult::Ok)
        {
            failures_.fetch_add(1, std::memory_order_relaxed);
            if (verbose_)
            {
                std::printf("[host] ! echo to %u failed: %s\n", connectionId,
                            std::string(ork::sdk::ToString(r)).c_str());
                std::fflush(stdout);
            }
            return;
        }
        echoed_.fetch_add(1, std::memory_order_relaxed);

        if (!verbose_) return;

        if (IsEchoMagic(payload)
            && payload.size() >= kEchoMagic.size() + kMarkerLen)
        {
            char marker[ORK_MARKER_LEN + 1] = {0};
            std::memcpy(marker, payload.data() + kEchoMagic.size(), ORK_MARKER_LEN);
            std::printf("[host] ~ echo %zu B to %u via %s (ORKBENCH1 marker=%s)\n",
                        payload.size(), connectionId, ChannelName(channel), marker);
        }
        else
        {
            std::printf("[host] ~ echo %zu B to %u via %s\n",
                        payload.size(), connectionId, ChannelName(channel));
        }
        std::fflush(stdout);
    }

    void OnStats(const ork::sdk::OrkStats& stats) override
    {
        // OnStats 约每秒一次；只缓存标量，别在回调里做重活。
        active_.store(stats.activeConnections, std::memory_order_relaxed);
        bytesIn_.store(stats.bytesIn,   std::memory_order_relaxed);
        bytesOut_.store(stats.bytesOut, std::memory_order_relaxed);
        packetsIn_.store(stats.packetsIn, std::memory_order_relaxed);
        cryptoFails_.store(CryptoFailTotal(stats), std::memory_order_relaxed);
    }

    // ---- 主线程读取的快照 ----
    uint64_t connections()      const noexcept { return connections_.load(std::memory_order_relaxed); }
    uint64_t disconnections()   const noexcept { return disconnections_.load(std::memory_order_relaxed); }
    uint64_t payloads()         const noexcept { return payloads_.load(std::memory_order_relaxed); }
    uint64_t echoed()           const noexcept { return echoed_.load(std::memory_order_relaxed); }
    uint64_t failures()         const noexcept { return failures_.load(std::memory_order_relaxed); }
    uint64_t active()           const noexcept { return active_.load(std::memory_order_relaxed); }
    uint64_t bytesIn()          const noexcept { return bytesIn_.load(std::memory_order_relaxed); }
    uint64_t bytesOut()         const noexcept { return bytesOut_.load(std::memory_order_relaxed); }
    uint64_t packetsIn()        const noexcept { return packetsIn_.load(std::memory_order_relaxed); }
    uint64_t cryptoFails()      const noexcept { return cryptoFails_.load(std::memory_order_relaxed); }
    uint32_t lastConnectionId() const noexcept { return lastConnectionId_.load(std::memory_order_relaxed); }

private:
    ork::sdk::ISender& sender_;
    const bool         verbose_;

    std::atomic<uint64_t> connections_{0};
    std::atomic<uint64_t> disconnections_{0};
    std::atomic<uint64_t> payloads_{0};
    std::atomic<uint64_t> echoed_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<uint64_t> active_{0};
    std::atomic<uint64_t> bytesIn_{0};
    std::atomic<uint64_t> bytesOut_{0};
    std::atomic<uint64_t> packetsIn_{0};
    std::atomic<uint64_t> cryptoFails_{0};
    std::atomic<uint32_t> lastConnectionId_{0};
};

// ============================================================================
// 客户端探针监听器（F10）
//
// demo 04 要打印每一帧的回显与 RTT，test_cpp_roundtrip 只要计数 —— 同样用
// verbose 开关区分。构造时就把两帧 25B 探针填好；每次重发前调
// Rebuild*Frame() 重盖时间戳，RTT 量的才是这一轮。
//
// 线程契约：OrkClient 没有内部线程，回调都在 PumpOnce() / Disconnect() 的
// 调用栈内触发。计数仍用 atomic —— 若宿主改用 Start() 派生 io 线程驱动，
// 同一份代码不需要重写。
// ============================================================================
class EchoProbeListener final : public ork::client::IClientListener
{
public:
    explicit EchoProbeListener(bool verbose = true) noexcept : verbose_(verbose)
    {
        RebuildReliableFrame();
        RebuildUnreliableFrame();
    }

    void OnConnected(uint32_t connectionId) override
    {
        connectionId_.store(connectionId, std::memory_order_relaxed);
        connected_.store(true, std::memory_order_relaxed);
        if (verbose_)
        {
            std::printf("[client] OnConnected  connectionId=%u\n", connectionId);
            std::fflush(stdout);
        }
    }

    // reason 只在 verbose 模式下打出来；两个调用方（04 / test_cpp_roundtrip）
    // 都只关心「断没断」，不为它单独留一个死成员（F12）。
    void OnDisconnected(uint32_t reason) override
    {
        disconnected_.store(true, std::memory_order_relaxed);
        if (verbose_)
        {
            std::printf("[client] OnDisconnected reason=%u\n", reason);
            std::fflush(stdout);
        }
    }

    void OnPayload(std::span<const uint8_t> data, TransportChannel channel) override
    {
        payloads_.fetch_add(1, std::memory_order_relaxed);
        const bool reliable = (channel == TransportChannel::Reliable);
        const std::span<const uint8_t> expect =
            reliable ? std::span<const uint8_t>(frameReliable_)
                     : std::span<const uint8_t>(frameUnreliable_);

        if (data.size() != kEchoFrameSize || !IsEchoMagic(data))
        {
            if (verbose_)
            {
                std::printf("[client] OnPayload %s %zu B —— 非 ORKBENCH1 帧\n",
                            reliable ? "Reliable" : "Unreliable", data.size());
                std::fflush(stdout);
            }
            return;
        }

        if (data.size() != expect.size()
            || std::memcmp(data.data(), expect.data(), expect.size()) != 0)
        {
            mismatches_.fetch_add(1, std::memory_order_relaxed);
            if (verbose_)
            {
                std::printf("[client] OnPayload %s echo MISMATCH\n",
                            reliable ? "Reliable" : "Unreliable");
                std::fflush(stdout);
            }
            return;
        }

        if (reliable) reliableEcho_.store(true, std::memory_order_relaxed);
        else          unreliableEcho_.store(true, std::memory_order_relaxed);

        if (verbose_)
        {
            const uint64_t sentNs = GetU64Le(data.data() + kEchoMagic.size() + kMarkerLen);
            const long long rttUs =
                static_cast<long long>((NowNs() - sentNs) / 1000ull);
            std::printf("[client] OnPayload %s echo OK (%zu B, rtt~%lld us)\n",
                        reliable ? "Reliable  " : "Unreliable", data.size(), rttUs);
            std::fflush(stdout);
        }
    }

    // ---- 主循环读写 ----
    bool     connected()        const noexcept { return connected_.load(std::memory_order_relaxed); }
    bool     disconnected()     const noexcept { return disconnected_.load(std::memory_order_relaxed); }
    bool     reliableEcho()     const noexcept { return reliableEcho_.load(std::memory_order_relaxed); }
    bool     unreliableEcho()   const noexcept { return unreliableEcho_.load(std::memory_order_relaxed); }
    uint32_t connectionId()     const noexcept { return connectionId_.load(std::memory_order_relaxed); }
    uint64_t payloads()         const noexcept { return payloads_.load(std::memory_order_relaxed); }
    uint64_t mismatches()       const noexcept { return mismatches_.load(std::memory_order_relaxed); }

    /** @brief 上一轮回显已消费，清掉标志准备下一轮 */
    void clearReliableEcho() noexcept { reliableEcho_.store(false, std::memory_order_relaxed); }

    std::span<const uint8_t> frameReliable()   const noexcept { return frameReliable_; }
    std::span<const uint8_t> frameUnreliable() const noexcept { return frameUnreliable_; }

    /** @brief 重盖发送时间戳（每次重发前调用，否则 RTT 量到上一轮） */
    void RebuildReliableFrame() noexcept
    {
        BuildEchoFrame(frameReliable_, kMarkerReliable);
    }
    void RebuildUnreliableFrame() noexcept
    {
        BuildEchoFrame(frameUnreliable_, kMarkerUnreliable);
    }

private:
    const bool verbose_;

    std::atomic<bool>     connected_{false};
    std::atomic<bool>     disconnected_{false};
    std::atomic<bool>     reliableEcho_{false};
    std::atomic<bool>     unreliableEcho_{false};
    std::atomic<uint32_t> connectionId_{0};
    std::atomic<uint64_t> payloads_{0};
    std::atomic<uint64_t> mismatches_{0};

    std::array<uint8_t, kEchoFrameSize> frameReliable_{};
    std::array<uint8_t, kEchoFrameSize> frameUnreliable_{};
};

// ============================================================================
// 双通道探针循环（F2）
//
// 04 / 06 / test_cpp_roundtrip 三处的 pump 循环节奏完全相同：连上之后先发
// 一帧 Reliable(KCP)，同时按 udpRetryMs 的间隔重试 Unreliable(UDP)，最多
// udpMaxAttempts 次（UDP 路由在握手刚完成时可能还没就绪，重发是正常做法）；
// 之后每收到一帧 Reliable 回显再发下一轮（天然限流）。
//
// 差别只在「怎么 pump、怎么发、怎么打印」，所以抽成 Driver 契约（duck typing，
// 不引入虚函数 —— C ABI 的 06 与 C++ SDK 的 04 都能适配）：
//
//   bool ShouldStop()      const  外部停止标志（如 Ctrl+C）；true 即结束
//   bool Pump()                   驱动一拍；返回 false 视为致命错误
//   void OnTick()                 每拍一次的额外动作（如打印状态迁移）
//   bool Connected()       const  是否已连上（未连上时只 pump 不发）
//   bool Disconnected()    const  是否已断开；true 即结束
//   bool ReliableEchoed()  const
//   bool UnreliableEchoed()const
//   void ClearReliableEcho()      清掉上一轮的回显标志
//   bool SendReliable(int round)      发一帧；返回是否入队成功，打印自理
//   bool SendUnreliable(int attempt)  同上
//
// 纯 C 的 02 用 demo_support_c.h 里的函数指针版
// ork_demo_run_dual_channel_probe()，循环节奏与此逐句对齐。
// ============================================================================
struct DualChannelProbeParams
{
    int reliableRounds = 1;                     ///< Reliable 探针轮数
    int timeoutMs      = 10000;                 ///< 整体超时
    int pumpSleepMs    = ORK_PUMP_SLEEP_MS;     ///< 每拍间隔
    int udpMaxAttempts = ORK_UDP_MAX_ATTEMPTS;  ///< Unreliable 最大重试次数
    int udpRetryMs     = ORK_UDP_RETRY_MS;      ///< Unreliable 重试间隔
};

struct DualChannelProbeResult
{
    bool reliableOk   = false;   ///< 全部轮数都发出且最后一轮回显到手
    bool unreliableOk = false;   ///< Unreliable 回显到手
    bool timedOut     = false;
    bool stopped      = false;   ///< ShouldStop() 触发（Ctrl+C）
    bool pumpFailed   = false;   ///< Pump() 返回 false
    int  reliableRounds = 0;     ///< 实际成功发出的轮数
    int  udpAttempts    = 0;     ///< 实际发出的 Unreliable 次数
};

template <class Driver>
DualChannelProbeResult RunDualChannelProbe(Driver& drv, const DualChannelProbeParams& p)
{
    DualChannelProbeResult res;
    const double deadline = NowMs() + static_cast<double>(p.timeoutMs);
    bool   firstRoundDone = false;
    int    rounds         = 0;
    int    udpAttempts    = 0;
    double nextUdpAt      = 0.0;

    for (;;)
    {
        const double t = NowMs();

        if (drv.ShouldStop()) { res.stopped = true; break; }
        // ★ 手动驱动：不调 Pump，连接流程一步都不会推进。
        if (!drv.Pump())      { res.pumpFailed = true; break; }
        drv.OnTick();

        if (drv.Connected())
        {
            // 第一轮：连接就绪后立刻发（此刻 KCP 会话已建好 —— OrkClient 先
            // StartKcpSession() 再把状态置为 Connected 并回调 OnConnected）。
            if (!firstRoundDone && drv.SendReliable(1))
            {
                firstRoundDone = true;
                rounds = 1;
            }

            if (!drv.UnreliableEchoed() && udpAttempts < p.udpMaxAttempts && t >= nextUdpAt)
            {
                drv.SendUnreliable(udpAttempts + 1);
                ++udpAttempts;
                nextUdpAt = t + static_cast<double>(p.udpRetryMs);
            }

            // 后续轮次：上一轮回显到手才发下一轮。
            if (firstRoundDone && drv.ReliableEchoed() && rounds < p.reliableRounds)
            {
                if (drv.SendReliable(rounds + 1)) ++rounds;
                drv.ClearReliableEcho();   // 等这一轮的回显
            }

            // 收敛：最后一轮 Reliable 回显到手 + Unreliable 有结果（或已到重试上限）
            if (rounds >= p.reliableRounds && drv.ReliableEchoed()
                && (drv.UnreliableEchoed() || udpAttempts >= p.udpMaxAttempts))
                break;
        }

        if (drv.Disconnected()) break;
        if (t >= deadline) { res.timedOut = true; break; }

        SleepMs(p.pumpSleepMs);
    }

    res.reliableRounds = rounds;
    res.udpAttempts    = udpAttempts;
    res.reliableOk = firstRoundDone && rounds >= p.reliableRounds && drv.ReliableEchoed();
    res.unreliableOk = drv.UnreliableEchoed();
    return res;
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
