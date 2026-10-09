#pragma once
/**
 * @file connection_rate_limiter.hpp
 * @brief A0.2：新连接速率限制器（per-IP token bucket + 全局 token bucket）
 *
 * Bootstrap Security 第八章"资源保护"的连接速率面：
 * - per-IP token bucket：burst 容量 + 持续补充速率（次/分钟），单 IP 连接风暴限流；
 * - 全局 token bucket：跨 IP 聚合的新连接接纳速率上限（分布式攻击最后防线）。
 *
 * 线程模型：**非线程安全**（单执行上下文）。所有调用必须与 TCP accept 完成
 * 回调在同一 asio::strand 内串行执行（TcpTransportServer 的 accept handler
 * 已绑定 NetworkPump strand，见 tcp_transport_server.cpp）。测试中可在单线程
 * 直接调用。
 *
 * 时间注入：构造函数接受 ClockFn（返回单调微秒），生产默认 steady_clock，
 * 测试可注入确定时间源（无 sleep、完全确定性）。
 *
 * IPv4/IPv6：按地址族字节精确区分（addrLen 4/16 不互相碰撞）；
 * 首版不做 IPv6 /64 聚合（文档 BootstrapSecurityDesign 第八章），
 * 也不把 v4-mapped IPv6（::ffff:x.x.x.x）归一化为 IPv4——asio to_v6()
 * 保留 mapped 形式，与 asio endpoint 相等语义一致。
 */

#include "ork/platform/types.hpp"
#include "ork/platform/time/time_defs.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>

// asio 依赖隔离：本头会进入 SDK 公共头链（session_interface.hpp →
// TransportConfig::connectionProtection），必须保持 asio-free（sdk_demo
// 等纯 Mock 接入不含 asio include 路径）。asio::ip::address 为非模板类，
// 此处前向声明即可；完整定义仅 .cpp 需要。
namespace asio {
namespace ip {
class address;
} // namespace ip
} // namespace asio

namespace ork::transport {

// =============================================================================
// 配置
// =============================================================================

/**
 * @brief 速率限制参数（token bucket × 2）
 *
 * 数值为"建议起点"（BootstrapSecurityDesign 第八章），生产经
 * NetworkConfig YAML 权威配置。
 */
struct ConnectionRateLimiterConfig
{
    /** per-IP burst 容量：单 IP 瞬时可连入的新连接数。默认 20。 */
    uint32_t perIpBurst{20};
    /** per-IP 持续补充速率（新连接数/分钟）。默认 60。 */
    uint32_t perIpRatePerMin{60};

    /**
     * 全局 burst 容量：跨 IP 聚合的瞬时新连接数。
     * 默认 100：覆盖服务器重启后 ≤100 客户端同时重连（线下赛事
     * 8 人房 × 12 房 ≈ 96，取整），不被正常重连风暴误伤。
     */
    uint32_t globalBurst{100};
    /**
     * 全局持续补充速率（新连接数/分钟）。默认 600（=10/s）：
     * - 下限依据：per-IP 默认 60/min 的 10 倍——需 ≥10 个不同 IP 同时
     *   以满速重连才触顶，不阻碍真实多用户接入；
     * - 上限依据：10/s 远低于 accept 循环消化能力与 TcpOnly 队列上限
     *   （默认 256）的回收速率，足以挡住把未完成 Bootstrap 队列瞬间
     *   打满的连接洪泛。
     */
    uint32_t globalRatePerMin{600};

    /**
     * per-IP 状态表尺寸上限：防随机伪造源 IP 撑爆 map。
     * 非四层保护配置面（YAML 不暴露）；内部防御参数，测试可注入小值。
     */
    size_t perIpMapCap{4096};
};

// =============================================================================
// 连接保护总配置（含并发上限，供 NetworkTransport 消费）
// =============================================================================

/**
 * @brief A0.2 连接接纳门禁总配置
 *
 * 四层保护：TCP 活跃总上限 → 未完成 Bootstrap（TcpOnly）上限 →
 * per-IP 速率 → 全局速率（判定顺序见 NetworkTransport::AdmitConnection）。
 * tcp_only_timeout_sec 独立于 connection_idle_timeout_sec：前者回收
 * 停留在 TcpOnly 的连接（默认 30s），后者回收全连接空闲。
 */
struct ConnectionProtectionConfig
{
    /** TCP 活跃连接（state != Disconnected）总上限。默认 2048。 */
    uint32_t maxActiveConnections{2048};
    /** 未完成 Bootstrap（TcpOnly/UdpHandshaking）连接上限。默认 256。 */
    uint32_t maxPendingBootstrap{256};
    /** TcpOnly 停留超时（秒）；0=禁用。默认 30。 */
    uint32_t tcpOnlyTimeoutSec{30};

    ConnectionRateLimiterConfig rateLimits;
};

// =============================================================================
// IpKey：per-IP 限流的定长可哈希键（不含端口）
// =============================================================================

/**
 * @brief IP 地址定长可哈希键（仅地址，不含端口）
 *
 * 与 connection/endpoint_key.hpp 的 EndpointKey 区别：本键**不含 port**——
 * per-IP 限流必须按地址聚合（源端口每个连接都不同，含 port 会退化成
 * 逐连接独立桶，限流失效）。字节布局与 EndpointKey 一致：
 * IPv4 → addrBytes[0..3]，addrLen=4；IPv6 → addrBytes[0..15]，addrLen=16。
 */
struct IpKey
{
    uint8_t addrBytes[16]{};
    uint8_t addrLen{0};

    /// 从 asio 地址构造（定义在 .cpp：需 asio 完整定义）
    static IpKey From(const asio::ip::address& addr);

    bool operator==(const IpKey& o) const
    {
        return addrLen == o.addrLen
            && std::memcmp(addrBytes, o.addrBytes, addrLen) == 0;
    }
};

/// FNV-1a（仅覆盖有效字节；addrLen 混入保证 v4/v6 不碰撞）
struct IpKeyHash
{
    size_t operator()(const IpKey& k) const
    {
        size_t h = 1469598103934665603ull;
        const auto mix = [&h](uint8_t b)
        {
            h ^= static_cast<size_t>(b);
            h *= 1099511628211ull;
        };
        mix(k.addrLen);
        for (uint8_t i = 0; i < k.addrLen; ++i)
            mix(k.addrBytes[i]);
        return h;
    }
};

// =============================================================================
// ConnectionRateLimiter
// =============================================================================

/**
 * @brief 新连接 token bucket 限流器（per-IP + 全局）
 *
 * 算法（整数微令牌，1 token = 1e6 微令牌，无浮点累积误差）：
 * - refill：tokens = min(burst, tokens + elapsedUs * ratePerMin / 60)
 *   （ratePerMin/60 = 每微秒补充的微令牌数）；
 * - tryConsume：refill 后 tokens >= 1e6 则扣 1 token 放行。
 *
 * per-IP 状态表：
 * - 过期清理：每 kSweepIntervalUs 全量扫描，删除最近消耗时间早于
 *   kPerIpIdleUs 的条目（惰性 + 周期混合，攻击者离开后状态自然回收）；
 * - 尺寸上限：插入新 IP 前若 size >= perIpMapCap 先做过期清理，
 *   仍满则拒绝该新 IP（已存在的 IP 不受影响，不增长 map）。
 */
class ConnectionRateLimiter
{
public:
    /// 时间源：返回单调微秒（生产 steady_clock；测试注入确定时间）
    using ClockFn = std::function<TimePoint()>;

    /// 生产默认时钟（与 MonotonicClock 同基准：steady_clock 微秒）
    static TimePoint DefaultClock()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @param config  限流参数
     * @param clock   时间源（可选；默认 steady_clock 微秒）
     */
    explicit ConnectionRateLimiter(const ConnectionRateLimiterConfig& config = {},
                                   ClockFn clock = &DefaultClock)
        : config_(config),
          clock_(std::move(clock)),
          global_(config.globalBurst)
    {
    }

    /**
     * @brief per-IP 令牌消耗（accept 门禁第 3 层）
     *
     * @return 该 IP 桶有令牌则消耗 1 并返回 true；否则 false
     *         （含 map 满 && 无过期可回收的新 IP 拒绝，归入 per-IP 拒绝类别）
     */
    bool TryConsumePerIp(const asio::ip::address& ip);

    /**
     * @brief 全局令牌消耗（accept 门禁第 4 层）
     *
     * @return 全局桶有令牌则消耗 1 并返回 true；否则 false
     */
    bool TryConsumeGlobal();

    /// per-IP 状态表当前条目数（诊断/测试）
    size_t PerIpEntryCount() const { return perIp_.size(); }

    /// 显式过期清理（测试用；生产走内部节流 sweep）
    void RemoveExpiredEntries(TimePoint now);

    /// 全局桶当前令牌数（向下取整；测试用）
    uint64_t GlobalTokens() const;

private:
    /// 单个 token bucket（整数微令牌）
    struct Bucket
    {
        explicit Bucket(uint32_t burst)
            : microTokens(static_cast<uint64_t>(burst) * kMicroPerToken),
              lastRefillUs(0),
              lastConsumeUs(0)
        {
        }
        Bucket() = default;

        /// 按流逝时间补充令牌（钳制 elapsed 防溢出），返回当前 now
        void Refill(TimePoint now, uint32_t burst, uint32_t ratePerMin);
        bool TryConsume(TimePoint now);

        uint64_t microTokens{0};
        TimePoint lastRefillUs{0};
        TimePoint lastConsumeUs{0};
    };

    static constexpr uint64_t kMicroPerToken = 1'000'000ull;
    /// sweep 节流间隔（微秒）：10s
    static constexpr int64_t kSweepIntervalUs = 10'000'000ll;
    /// per-IP 条目空闲过期（微秒）：5min 无消耗即回收
    static constexpr int64_t kPerIpIdleUs = 300'000'000ll;
    /// refill 钳制的最大 elapsed（微秒）：1h（超过则必然已回满）
    static constexpr int64_t kMaxElapsedUs = 3'600'000'000ll;

    ConnectionRateLimiterConfig config_;
    ClockFn clock_;
    Bucket global_;
    std::unordered_map<IpKey, Bucket, IpKeyHash> perIp_;
    TimePoint lastSweepUs_{0};
    bool clockInitialized_{false};
};

}  // namespace ork::transport
