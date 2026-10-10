/**
 * @file demo_support_c.h
 * @brief 纯 C11 共享层 —— Ork SDK C/C++ demo 与测试的公共小工具
 *
 * 设计约束
 * --------
 *   - 全部为 `static inline` / 宏，**不引入任何链接期依赖**，
 *     拷走一个 demo 目录 + 本头文件即可独立编译；
 *   - 只依赖 C11 标准库 + POSIX/Win32 socket + Ork 的 C ABI 头；
 *   - C++ demo **不直接**用本文件，而是通过 demo_support.hpp 的薄封装
 *     （这样枚举名、探针常量、配置填充只有一份权威定义）。
 *
 * 内容
 * ----
 *   - 计时 / 睡眠        ork_demo_now_ms, ork_demo_now_ns, ork_demo_sleep_ms
 *   - 字节序             ork_demo_put_u64_le, ork_demo_get_u64_le
 *   - 25B 探针帧         ork_demo_build_frame, ork_demo_is_echo_magic, ork_demo_hexdump
 *   - 枚举名             ork_demo_send_result_name, ork_demo_disconnect_reason_name,
 *                        ork_demo_client_state_name, ork_demo_channel_name, ork_demo_link_name
 *   - 配置填充 (F8)      ork_demo_fill_host_config, ork_demo_fill_client_config
 *   - 加密失败合计 (F9)  ork_demo_crypto_fail_total
 *   - 临时端口           ork_demo_pick_free_port, ork_demo_pick_closed_tcp_port
 *   - 双通道探针 (F2)    ork_demo_run_dual_channel_probe
 *   - 环境 / 横幅 (F19)  ork_demo_env_or, ork_demo_env_log_level, ork_demo_print_banner
 */

#ifndef ORK_DEMO_SUPPORT_C_H
#define ORK_DEMO_SUPPORT_C_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  define ORK_DEVNULL "NUL"
#  define ORK_CLOSESOCK(fd) closesocket(fd)
   typedef SOCKET ork_sock_t;
#  define ORK_SOCK_INVALID INVALID_SOCKET
#else
#  include <time.h>
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  define ORK_DEVNULL "/dev/null"
#  define ORK_CLOSESOCK(fd) close(fd)
   typedef int ork_sock_t;
#  define ORK_SOCK_INVALID (-1)
#endif

/* Ork C ABI —— 提供 OrkSendResult / OrkDisconnectReason / OrkClientState /
 * OrkChannel / OrkLinkType / OrkLogLevel / OrkHostConfig / OrkClientConfig /
 * OrkStats 等类型。 */
#include "ork/sdk/c/ork_abi.h"
#include "ork/sdk/c/ork_client_abi.h"

/* ==========================================================================
 * 25B opaque 回显探针帧协议常量
 *
 *   magic(9B "ORKBENCH1") + marker(8B) + 单调时间戳 ns(8B, 小端) = 25B
 *
 * 预编译服务端 embed_minimal 的 TransportDemoHandler 对「长度恰为 25 且
 * 前缀 ORKBENCH1」的 payload 走 opaque 原样回显路径，不尝试 protobuf 解析。
 * ========================================================================== */
#define ORK_ECHO_FRAME_SIZE   25u
#define ORK_ECHO_MAGIC        "ORKBENCH1"
#define ORK_ECHO_MAGIC_LEN    9u
#define ORK_MARKER_RELIABLE   "ORKDEMO1"
#define ORK_MARKER_UNRELIABLE "ORKDEMO2"
#define ORK_MARKER_LEN        8u

/* ==========================================================================
 * 双通道探针参数
 *
 * Unreliable 走裸 UDP，回显可能丢，所以按 ORK_UDP_RETRY_MS 的间隔重试，
 * 上限 ORK_UDP_MAX_ATTEMPTS 次 —— 8 × 150ms = 1.2s，对 loopback 足够。
 * ========================================================================== */
#define ORK_UDP_MAX_ATTEMPTS  8
#define ORK_UDP_RETRY_MS      150
#define ORK_PUMP_SLEEP_MS     5

/* ==========================================================================
 * 计时 / 睡眠
 * ========================================================================== */

/** @brief 单调时钟毫秒（与 steady_clock 同源，可直接用于 RTT 计算） */
static inline uint64_t ork_demo_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/** @brief 单调时钟纳秒 —— 探针帧里的时间戳字段 */
static inline uint64_t ork_demo_now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    if (freq.QuadPart == 0) return 0;
    return (uint64_t)((double)cnt.QuadPart * 1.0e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ork_demo_sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ==========================================================================
 * 字节序
 * ========================================================================== */
static inline void ork_demo_put_u64_le(uint8_t* dst, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i) dst[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

static inline uint64_t ork_demo_get_u64_le(const uint8_t* src)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i) v = (v << 8) | (uint64_t)src[i];
    return v;
}

/* ==========================================================================
 * 探针帧组装 / 识别
 * ========================================================================== */

/**
 * @brief 组装一个 25B ORKBENCH1 探针帧
 * @param out    目标缓冲，至少 ORK_ECHO_FRAME_SIZE 字节
 * @param marker 8 字节标记（ORK_MARKER_RELIABLE / ORK_MARKER_UNRELIABLE）
 */
static inline void ork_demo_build_frame(uint8_t* out, const char* marker)
{
    memcpy(out, ORK_ECHO_MAGIC, ORK_ECHO_MAGIC_LEN);
    memcpy(out + ORK_ECHO_MAGIC_LEN, marker, ORK_MARKER_LEN);
    ork_demo_put_u64_le(out + ORK_ECHO_MAGIC_LEN + ORK_MARKER_LEN, ork_demo_now_ns());
}

/** @brief payload 前缀是否为 ORKBENCH1（长度不足直接判否） */
static inline int ork_demo_is_echo_magic(const uint8_t* data, uint32_t size)
{
    return size >= ORK_ECHO_MAGIC_LEN
        && memcmp(data, ORK_ECHO_MAGIC, ORK_ECHO_MAGIC_LEN) == 0;
}

/** @brief 取出帧内 marker（不含 NUL），size 不足时返回 NULL */
static inline const char* ork_demo_frame_marker(const uint8_t* data, uint32_t size)
{
    if (size < ORK_ECHO_MAGIC_LEN + ORK_MARKER_LEN) return NULL;
    return (const char*)data + ORK_ECHO_MAGIC_LEN;
}

static inline void ork_demo_hexdump(const uint8_t* data, uint32_t size)
{
    uint32_t i;
    for (i = 0; i < size; ++i) printf("%02x", data[i]);
}

/* ==========================================================================
 * 枚举名映射
 * ========================================================================== */
static inline const char* ork_demo_send_result_name(OrkSendResult r)
{
    switch (r)
    {
    case OrkSend_Ok:           return "Ok";
    case OrkSend_TooLarge:     return "TooLarge";
    case OrkSend_NotConnected: return "NotConnected";
    case OrkSend_BadState:     return "BadState";
    case OrkSend_QueueFull:    return "QueueFull";
    }
    return "Unknown";
}

static inline const char* ork_demo_disconnect_reason_name(OrkDisconnectReason r)
{
    switch (r)
    {
    case OrkDisconnect_PeerClosed:       return "PeerClosed";
    case OrkDisconnect_Idle:             return "IdleTimeout";
    case OrkDisconnect_Kicked:           return "Kicked";
    case OrkDisconnect_ProtocolError:    return "ProtocolError";
    case OrkDisconnect_Internal:         return "Internal";
    case OrkDisconnect_BootstrapTimeout: return "BootstrapTimeout";
    case OrkDisconnect_MaxConnections:   return "MaxConnections";
    case OrkDisconnect_Unknown:          break;
    }
    return "Unknown";
}

static inline const char* ork_demo_client_state_name(OrkClientState s)
{
    switch (s)
    {
    case OrkClientState_Disconnected:  return "Disconnected";
    case OrkClientState_Connecting:    return "Connecting";
    case OrkClientState_Bootstrapping: return "Bootstrapping";
    case OrkClientState_Handshaking:   return "Handshaking";
    case OrkClientState_Connected:     return "Connected";
    case OrkClientState_Disconnecting: return "Disconnecting";
    }
    return "Unknown";
}

static inline const char* ork_demo_channel_name(OrkChannel ch)
{
    return ch == OrkChannel_Reliable ? "Reliable" : "Unreliable";
}

static inline const char* ork_demo_link_name(OrkLinkType t)
{
    switch (t)
    {
    case OrkLink_Tcp: return "TCP";
    case OrkLink_Udp: return "UDP";
    case OrkLink_Kcp: return "KCP";
    }
    return "?";
}

/* ==========================================================================
 * 配置填充（F8）
 *
 * INVARIANTS（唯一权威定义处，全仓不再重复解释）：
 *   - tcp_port == udp_port：服务端 TCP bootstrap 与 UDP/KCP 共用同一 gamePort；
 *     客户端 udp_port 留 0 会回落到默认 9401，必然连不上。
 *   - drive_mode 必须为 0（Internal）：External 模式会被 ork_host_create 拒绝。
 *   - kcp_update_rate_hz >= pump_rate_hz：否则 Create 校验失败。
 *   - kcp_stream_mode 必须为 0：OrkFrame 分帧与 stream 模式不兼容。
 *   - max_packet_size ∈ [50, 1414]：1200 是与 server/config/network.yaml
 *     对齐的 demo 默认值。
 *   - bootstrap_security_mode 恒 PlaintextDevelopment：明文 bootstrap 只在
 *     -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 的构建里编入，且 bind 被强制收敛
 *     到 loopback。生产请改用 TLS。
 * ========================================================================== */

/** @brief 填充 OrkHostConfig（C ABI 服务端） */
static inline void ork_demo_fill_host_config(OrkHostConfig* c, uint16_t port,
                                             int enable_encryption)
{
    memset(c, 0, sizeof(*c));
    c->struct_size              = (uint32_t)sizeof(OrkHostConfig);
    c->tcp_port                 = port;
    c->udp_port                 = port;              /* ★ 与 tcp_port 相同 */
    c->enable_kcp               = 1;
    c->enable_encryption        = enable_encryption ? 1 : 0;
    c->bootstrap_ttl_sec        = 300;
    c->pump_rate_hz             = 30;
    c->kcp_update_rate_hz       = 100;               /* ★ >= pump_rate_hz */
    c->idle_timeout_sec         = 300;
    c->max_packet_size          = 1200;
    c->outbound_queue_capacity  = 0;                 /* 0 = 库默认 */
    c->inbound_queue_capacity   = 0;
    c->drive_mode               = 0;                 /* ★ Internal */
    c->log_path                 = ORK_DEVNULL;
    c->log_level                = (uint32_t)OrkLog_Warn;
}

/**
 * @brief 填充 OrkClientConfig（C ABI 客户端，v2 布局）
 * @param connect_timeout_ms  <= 0 时回落到 5000；tls_handshake_timeout_ms 跟随它
 */
static inline void ork_demo_fill_client_config(OrkClientConfig* c, uint16_t port,
                                               int enable_encryption,
                                               int connect_timeout_ms)
{
    const uint32_t timeout = (uint32_t)(connect_timeout_ms > 0 ? connect_timeout_ms : 5000);

    memset(c, 0, sizeof(*c));
    c->struct_size                = (uint32_t)sizeof(OrkClientConfig);  /* v2 = 96 */
    c->host                       = "127.0.0.1";
    c->tcp_port                   = port;
    c->udp_port                   = port;             /* ★ 必须等于 tcp_port */
    c->enable_kcp                 = 1;
    c->enable_encryption          = enable_encryption ? 1 : 0;
    c->max_packet_size            = 1200;
    c->connect_timeout_ms         = timeout;
    c->kcp_stream_mode            = 0;                /* ★ OrkFrame 分帧不兼容 stream */
    c->bootstrap_security_mode    = (int)OrkBootstrapSecurity_PlaintextDev;
    c->tls_ca_file                = NULL;
    c->tls_expected_server_name   = NULL;
    c->tls_use_system_trust_store = 0;
    c->tls_handshake_timeout_ms   = timeout;
}

/* ==========================================================================
 * 加密失败合计（F9）
 * ========================================================================== */
static inline unsigned long long ork_demo_crypto_fail_total(const OrkStats* s)
{
    return (unsigned long long)s->crypto_fail_aead
         + (unsigned long long)s->crypto_fail_replay
         + (unsigned long long)s->crypto_fail_expired
         + (unsigned long long)s->crypto_fail_malformed;
}

/* ==========================================================================
 * 临时端口
 * ========================================================================== */

#ifdef _WIN32
/** @brief Winsock 要显式初始化；只做一次（WSAStartup 本身可重入，但没必要） */
static inline void ork_demo_wsa_init(void)
{
    static LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) == 0)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
}
#else
static inline void ork_demo_wsa_init(void) { }
#endif

/**
 * @brief 申请一个 TCP 与 UDP 都空闲的 loopback 端口
 *
 * 返回后 socket 立即关闭释放端口，理论上存在被抢占的竞态窗口，
 * 对单机 demo / ctest 足够。返回 0 表示 64 次尝试都失败。
 */
static inline uint16_t ork_demo_pick_free_port(void)
{
    int attempt;
    ork_demo_wsa_init();
    for (attempt = 0; attempt < 64; ++attempt)
    {
        struct sockaddr_in addr;
        socklen_t addrlen = (socklen_t)sizeof(addr);
        uint16_t port;

        ork_sock_t udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp == ORK_SOCK_INVALID) continue;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;                     /* 让内核挑 */
        if (bind(udp, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        { ORK_CLOSESOCK(udp); continue; }
        if (getsockname(udp, (struct sockaddr*)&addr, &addrlen) != 0)
        { ORK_CLOSESOCK(udp); continue; }

        port = (uint16_t)ntohs(addr.sin_port);
        ORK_CLOSESOCK(udp);

        /* 同一端口再探 TCP —— 服务端 bootstrap 与 KCP 共用它 */
        {
            ork_sock_t tcp = socket(AF_INET, SOCK_STREAM, 0);
            if (tcp == ORK_SOCK_INVALID) continue;

            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port        = htons(port);
            if (bind(tcp, (struct sockaddr*)&addr, sizeof(addr)) != 0)
            { ORK_CLOSESOCK(tcp); continue; }
            ORK_CLOSESOCK(tcp);
        }
        return port;
    }
    return 0;
}

/**
 * @brief 申请一个**已关闭**的 TCP 端口（用于「连接必然被 RST 拒绝」的用例）
 *
 * 与 ork_demo_pick_free_port 的区别：只探 TCP，且返回前一定关闭，
 * 目的是让后续 connect() 立刻收到 ECONNREFUSED，而不是被别的进程占走。
 */
static inline uint16_t ork_demo_pick_closed_tcp_port(void)
{
    int attempt;
    ork_demo_wsa_init();
    for (attempt = 0; attempt < 64; ++attempt)
    {
        struct sockaddr_in addr;
        socklen_t addrlen = (socklen_t)sizeof(addr);
        uint16_t port;

        ork_sock_t tcp = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp == ORK_SOCK_INVALID) continue;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (bind(tcp, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        { ORK_CLOSESOCK(tcp); continue; }
        if (getsockname(tcp, (struct sockaddr*)&addr, &addrlen) != 0)
        { ORK_CLOSESOCK(tcp); continue; }

        port = (uint16_t)ntohs(addr.sin_port);
        ORK_CLOSESOCK(tcp);
        return port;
    }
    return 0;
}

/* ==========================================================================
 * 环境变量 / 横幅（F19）
 * ========================================================================== */
static inline const char* ork_demo_env_or(const char* name, const char* fallback)
{
    const char* v = getenv(name);
    return (v != NULL && *v != '\0') ? v : fallback;
}

/**
 * @brief 解析 ORK_LOG_LEVEL；无法识别时打一行诊断并回落到 Warn
 * @param who 日志前缀（如 "host" / "client"），仅用于诊断输出
 */
static inline uint32_t ork_demo_env_log_level(const char* who)
{
    const char* v = getenv("ORK_LOG_LEVEL");
    if (v == NULL || *v == '\0') return (uint32_t)OrkLog_Warn;
    if (strcmp(v, "trace") == 0) return (uint32_t)OrkLog_Trace;
    if (strcmp(v, "debug") == 0) return (uint32_t)OrkLog_Debug;
    if (strcmp(v, "info")  == 0) return (uint32_t)OrkLog_Info;
    if (strcmp(v, "warn")  == 0) return (uint32_t)OrkLog_Warn;
    if (strcmp(v, "error") == 0) return (uint32_t)OrkLog_Error;
    if (strcmp(v, "off")   == 0) return (uint32_t)OrkLog_Off;
    fprintf(stderr, "[%s] 忽略无法识别的 ORK_LOG_LEVEL=\"%s\"，回落到 warn\n", who, v);
    return (uint32_t)OrkLog_Warn;
}

static inline const char* ork_demo_env_log_file(void)
{
    return ork_demo_env_or("ORK_LOG_FILE", ORK_DEVNULL);
}

/** @brief 横幅分隔线；需要自己往横幅里塞额外信息行时直接用它 */
#define ORK_BANNER_RULE "============================================================="

/** @brief 打印「分隔线 + 标题 + 分隔线」三行横幅 */
static inline void ork_demo_print_banner(const char* title)
{
    printf(ORK_BANNER_RULE "\n");
    printf(" %s\n", title);
    printf(ORK_BANNER_RULE "\n");
}

/* ==========================================================================
 * 双通道探针驱动（F2）—— 函数指针版，供纯 C demo 使用
 *
 * 封装的模式：
 *   pump → 连上后发第 1 轮 Reliable → 以 udp_retry_ms 间隔重试 Unreliable
 *   （上限 udp_max_attempts 次）→ 每收到一次 Reliable 回显发下一轮
 *   → 最后一轮回显到手且 Unreliable 有结论 → 收敛退出
 *
 * C++ 侧的等价物是 demo_support.hpp 的 RunDualChannelProbe<Driver>。
 * ========================================================================== */

/** @brief 探针需要读写的连接状态（由调用方的回调维护） */
typedef struct OrkDemoProbeState
{
    int connected;          /**< OnConnected 已触发 */
    int disconnected;       /**< OnDisconnected 已触发 */
    int first_send_ok;      /**< 第 1 轮 Reliable 已成功入队 */
    int reliable_echo;      /**< Reliable 回显到手；驱动消费后清 0 */
    int unreliable_echo;    /**< Unreliable 回显到手 */
} OrkDemoProbeState;

/** @brief 探针与具体传输之间的适配层 */
typedef struct OrkDemoProbeDriver
{
    void*              ctx;               /**< 透传给各回调（如 OrkClientHandle） */
    OrkDemoProbeState* state;             /**< 必填 */
    uint8_t*           frame_reliable;    /**< 必填，ORK_ECHO_FRAME_SIZE 字节 */
    uint8_t*           frame_unreliable;  /**< 必填，ORK_ECHO_FRAME_SIZE 字节 */

    OrkError (*pump)(void* ctx);          /**< 必填；返回非 OrkOk 即终止 */

    /**
     * @brief 必填。把 frame 发出去。
     *
     * frame 是**可写**的：每次发送前都应该重新盖时间戳
     * （`ork_demo_build_frame(frame, marker)`），否则 RTT 会量到上一轮的值。
     */
    OrkSendResult (*send)(void* ctx, uint8_t* frame, uint32_t size,
                          OrkChannel channel);

    /* 以下均可选（NULL = 不启用） */
    OrkClientState (*get_state)(void* ctx);
    void (*on_state_change)(void* ctx, OrkClientState prev, OrkClientState cur);
    int  (*should_stop)(void* ctx);       /**< 如 Ctrl+C 标志 */
    void (*log_send)(void* ctx, OrkChannel channel, int index, OrkSendResult r);
} OrkDemoProbeDriver;

typedef struct OrkDemoProbeParams
{
    int reliable_rounds;              /**< 期望的 Reliable 轮数（>= 1） */
    int timeout_ms;                   /**< 整体预算 */
    int pump_sleep_ms;                /**< 每拍睡眠，默认 ORK_PUMP_SLEEP_MS */
    int udp_max_attempts;             /**< 默认 ORK_UDP_MAX_ATTEMPTS */
    int udp_retry_ms;                 /**< 默认 ORK_UDP_RETRY_MS */
    int first_round_sent_in_callback; /**< 1 = 第 1 轮已由 OnConnected 发出 */
} OrkDemoProbeParams;

typedef struct OrkDemoProbeResult
{
    int reliable_ok;        /**< 全部轮次都发出去且最后一轮回显到手 */
    int unreliable_ok;      /**< Unreliable 回显到手 */
    int reliable_rounds;    /**< 实际完成的轮数 */
    int udp_attempts;       /**< 实际 Unreliable 尝试次数 */
    int timed_out;
    int stopped;
    int pump_failed;
} OrkDemoProbeResult;

/** @brief 填充除 first_round_sent_in_callback 外的合理默认，避免调用方漏填 */
static inline OrkDemoProbeParams ork_demo_probe_params_default(int reliable_rounds,
                                                               int timeout_ms)
{
    OrkDemoProbeParams p;
    memset(&p, 0, sizeof(p));
    p.reliable_rounds              = reliable_rounds > 0 ? reliable_rounds : 1;
    p.timeout_ms                   = timeout_ms > 0 ? timeout_ms : 10000;
    p.pump_sleep_ms                = ORK_PUMP_SLEEP_MS;
    p.udp_max_attempts             = ORK_UDP_MAX_ATTEMPTS;
    p.udp_retry_ms                 = ORK_UDP_RETRY_MS;
    p.first_round_sent_in_callback = 0;
    return p;
}

static inline OrkDemoProbeResult
ork_demo_run_dual_channel_probe(const OrkDemoProbeDriver* d,
                                const OrkDemoProbeParams* p)
{
    OrkDemoProbeResult res;
    const uint64_t deadline = ork_demo_now_ms() + (uint64_t)p->timeout_ms;
    int first_round_done = 0;
    int rounds           = 0;
    int udp_attempts     = 0;
    uint64_t next_udp_at = 0;
    OrkClientState last_state = OrkClientState_Disconnected;

    memset(&res, 0, sizeof(res));

    for (;;)
    {
        const uint64_t now = ork_demo_now_ms();

        if (d->should_stop != NULL && d->should_stop(d->ctx)) { res.stopped = 1; break; }

        if (d->pump(d->ctx) != OrkOk) { res.pump_failed = 1; break; }

        if (d->get_state != NULL)
        {
            const OrkClientState st = d->get_state(d->ctx);
            if (st != last_state)
            {
                if (d->on_state_change != NULL) d->on_state_change(d->ctx, last_state, st);
                last_state = st;
            }
        }

        if (d->state->connected)
        {
            /* 第 1 轮：要么 OnConnected 回调里已经发过，要么在这里发 */
            if (!first_round_done)
            {
                if (p->first_round_sent_in_callback)
                {
                    if (d->state->first_send_ok) { first_round_done = 1; rounds = 1; }
                }
                else
                {
                    const OrkSendResult r =
                        d->send(d->ctx, d->frame_reliable, ORK_ECHO_FRAME_SIZE,
                                OrkChannel_Reliable);
                    if (d->log_send != NULL)
                        d->log_send(d->ctx, OrkChannel_Reliable, 1, r);
                    if (r == OrkSend_Ok) { first_round_done = 1; rounds = 1; }
                }
            }

            /* Unreliable：裸 UDP 可能丢，按固定间隔重试 */
            if (!d->state->unreliable_echo && udp_attempts < p->udp_max_attempts
                && now >= next_udp_at)
            {
                const OrkSendResult r =
                    d->send(d->ctx, d->frame_unreliable, ORK_ECHO_FRAME_SIZE,
                            OrkChannel_Unreliable);
                ++udp_attempts;
                next_udp_at = now + (uint64_t)p->udp_retry_ms;
                if (d->log_send != NULL)
                    d->log_send(d->ctx, OrkChannel_Unreliable, udp_attempts, r);
            }

            /* 后续 Reliable 轮次：上一轮回显到手才发下一轮（天然限流） */
            if (first_round_done && d->state->reliable_echo && rounds < p->reliable_rounds)
            {
                const OrkSendResult r =
                    d->send(d->ctx, d->frame_reliable, ORK_ECHO_FRAME_SIZE,
                            OrkChannel_Reliable);
                if (r == OrkSend_Ok) ++rounds;
                d->state->reliable_echo = 0;
                if (d->log_send != NULL)
                    d->log_send(d->ctx, OrkChannel_Reliable, rounds, r);
            }

            /* 收敛：最后一轮回显到手 + Unreliable 已有结论 */
            if (rounds >= p->reliable_rounds && d->state->reliable_echo
                && (d->state->unreliable_echo || udp_attempts >= p->udp_max_attempts))
                break;
        }

        if (d->state->disconnected) break;
        if (now >= deadline) { res.timed_out = 1; break; }

        ork_demo_sleep_ms(p->pump_sleep_ms);
    }

    res.reliable_rounds = rounds;
    res.udp_attempts    = udp_attempts;
    res.reliable_ok     = first_round_done && rounds >= p->reliable_rounds
                          && d->state->reliable_echo;
    res.unreliable_ok   = d->state->unreliable_echo;
    return res;
}

#endif /* ORK_DEMO_SUPPORT_C_H */
