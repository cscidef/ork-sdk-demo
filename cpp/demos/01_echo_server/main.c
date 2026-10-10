/**
 * @file main.c
 * @brief Demo 01 —— 纯 C11 的最小回显服务端（C ABI Host）
 *
 * 展示内容
 * --------
 * 只用一个头文件 `<ork/sdk/c/ork_abi.h>` 就把 Ork 的完整服务端传输栈跑起来：
 *
 *   1. `ORK_HOST_CONFIG_INIT` 复合字面量填好 `struct_size`，其余字段按需覆盖；
 *   2. `ork_host_create` 建立宿主（内部完成参数校验 + SessionFramework::Create）；
 *   3. 注册四个 C 函数指针回调：on_connected / on_disconnected / on_payload / on_stats；
 *   4. `ork_host_start` 拉起内部 pump 线程（drive_mode = 0 = Internal）；
 *   5. 主循环 `ork_host_pump_once` 做协作式让出（约 1ms），顺便打印心跳；
 *   6. SIGINT / SIGTERM 触发 `ork_host_stop` + `ork_host_destroy` 干净退出。
 *
 * 回显语义：`on_payload` 把收到的字节 **原样、同通道** 送回去，
 * 与预编译 demo 服务端（server/macos-arm64/bin/embed_minimal）的
 * `TransportDemoHandler` opaque 回显路径一致。因此 02_echo_client /
 * python 侧的 demo 连本进程和连 embed_minimal 表现完全相同。
 *
 * 线程契约（很重要）
 * ------------------
 * 回调全部在 **内部 pump 线程** 上触发：
 *   - `ork_host_send` / `ork_host_broadcast` / `ork_host_disconnect`
 *     在回调里调用是安全的（只是把 payload 拷进出站队列 / 异步 close）；
 *   - `ork_host_stop` / `ork_host_destroy` **绝对不能**在回调里调用 ——
 *     它们要 join 当前这条 pump 线程，会自死锁。
 *
 * 安全模式
 * --------
 * C ABI Host 恒定使用 `BootstrapSecurityMode::PlaintextDevelopment`
 * （见 ork_abi.cpp 的 BuildTransportConfig），因此：
 *   - 需要 SDK 以 `-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON` 构建（本仓随包库即是）；
 *   - bind 被强制收敛到 loopback，只能本机客户端连接；
 *   - 生产环境请改用 C++ SDK（见 demo 03）并配置 TLS 1.3 证书。
 *
 * 用法
 * ----
 *   ./01_echo_server              # 监听 9400
 *   ./01_echo_server 9500         # 监听 9500
 *   ORK_LOG_LEVEL=debug ORK_LOG_FILE=/tmp/ork.log ./01_echo_server
 *
 * 环境变量
 *   ORK_LOG_FILE   Ork 内部日志文件路径（默认 /dev/null，即丢弃）
 *   ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）
 */

/* demo_support_c.h 已经带进 ork_abi.h / ork_client_abi.h、平台差异宏
 * （ORK_DEVNULL 等）与全部共享小工具（计时、枚举名、探针帧协议常量）。 */
#include "demo_support_c.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* 跨线程共享状态                                                              */
/*                                                                            */
/* 写者是内部 pump 线程（回调），读者是主线程（心跳打印）—— 这是货真价实的      */
/* 数据竞争，所以一律用 _Atomic（F26）：volatile 在 C11 里只约束「不被优化掉」， */
/* 不提供任何原子性 / 内存序保证；64 位计数器在非 x86 上还可能被撕裂读。        */
/* g_stop 用 _Atomic sig_atomic_t：C11 只保证它在信号处理器里是 lock-free 的。  */
/* -------------------------------------------------------------------------- */
static OrkHost               g_host           = NULL;  /* 回调里也要用，故置全局 */
static _Atomic sig_atomic_t  g_stop           = 0;
static _Atomic uint64_t      g_payloads       = 0;     /* 收到的 payload 数 */
static _Atomic uint64_t      g_echoed         = 0;     /* 成功回显的 payload 数 */
static _Atomic uint64_t      g_send_failures  = 0;
static _Atomic uint64_t      g_connections    = 0;     /* 累计接入数 */
static _Atomic uint64_t      g_disconnections = 0;
static _Atomic uint64_t      g_bytes_in       = 0;     /* 最近一次 on_stats 快照 */
static _Atomic uint64_t      g_bytes_out      = 0;
static _Atomic uint64_t      g_packets_in     = 0;
static _Atomic uint32_t      g_active         = 0;

/* -------------------------------------------------------------------------- */
/* 小工具（计时 / 枚举名 / 环境变量解析全部来自 demo_support_c.h）              */
/* -------------------------------------------------------------------------- */
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;   /* 只做置位；真正的 stop/destroy 在主线程执行 */
}

static void print_status(double uptime_ms)
{
    printf("[host] up=%.1fs  active=%u  conn(+%llu/-%llu)  payload=%llu  echoed=%llu"
           "  send_fail=%llu  in=%llu B / %llu pkt  out=%llu B\n",
           uptime_ms / 1000.0,
           g_active,
           (unsigned long long)g_connections,
           (unsigned long long)g_disconnections,
           (unsigned long long)g_payloads,
           (unsigned long long)g_echoed,
           (unsigned long long)g_send_failures,
           (unsigned long long)g_bytes_in,
           (unsigned long long)g_packets_in,
           (unsigned long long)g_bytes_out);
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/* 回调（内部 pump 线程）                                                      */
/* -------------------------------------------------------------------------- */
static void on_connected(void* user, uint32_t connection_id)
{
    (void)user;
    g_connections++;

    OrkConnectionInfo info = ORK_CONNECTION_INFO_INIT;
    const int have_info = (g_host != NULL)
        && (ork_host_get_connection_info(g_host, connection_id, &info) == OrkOk);

    printf("[host] + connection %u  active=%u  link=%u  encryption=%s\n",
           connection_id,
           ork_host_connection_count(g_host),
           have_info ? info.link : 0u,
           (have_info && info.encryption_active) ? "on" : "off");
    fflush(stdout);
}

static void on_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    (void)user;
    g_disconnections++;
    printf("[host] - connection %u  reason=%s(%u)  active=%u\n",
           connection_id,
           ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason,
           ork_host_connection_count(g_host));
    fflush(stdout);
}

static void on_payload(void* user, uint32_t connection_id,
                       const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)user;
    g_payloads++;

    /* 回显：原样、同通道送回。
     * 注意 data 的生命周期 **仅限本次回调**（指向 core 内部缓冲），
     * ork_host_send 返回后即可失效 —— 它内部已经拷贝过了。 */
    const OrkSendResult r =
        ork_host_send(g_host, connection_id, data, size, (OrkChannel)channel);

    if (r != OrkSend_Ok)
    {
        g_send_failures++;
        printf("[host] ! echo to %u failed: %s\n",
               connection_id, ork_demo_send_result_name(r));
        fflush(stdout);
        return;
    }

    g_echoed++;

    /* 25B "ORKBENCH1" 帧是 Ork 各语言 demo 通用的 opaque 回显探针
     * （帧头 ORK_ECHO_MAGIC_LEN + ORK_MARKER_LEN = 17B，再加 8B 时间戳共 25B），
     * 这里把 marker 打出来方便和 python / C++ 侧对照。 */
    if (ork_demo_is_echo_magic(data, size))
    {
        char marker[ORK_MARKER_LEN + 1];
        memcpy(marker, ork_demo_frame_marker(data, size), ORK_MARKER_LEN);
        marker[ORK_MARKER_LEN] = '\0';
        printf("[host] ~ echo %u B to %u via %s (ORKBENCH1 marker=%s)\n",
               size, connection_id,
               ork_demo_channel_name((OrkChannel)channel), marker);
    }
    else
    {
        printf("[host] ~ echo %u B to %u via %s\n",
               size, connection_id, ork_demo_channel_name((OrkChannel)channel));
    }
    fflush(stdout);
}

static void on_stats(void* user, const OrkStats* stats)
{
    (void)user;
    if (stats == NULL) return;
    /* 只取标量字段，避免整个结构体跨线程拷贝时的撕裂读。 */
    g_active     = stats->active_connections;
    g_bytes_in   = stats->bytes_in;
    g_bytes_out  = stats->bytes_out;
    g_packets_in = stats->packets_in;
}

/* -------------------------------------------------------------------------- */
static void usage(const char* prog)
{
    printf("用法: %s [port] [--help]\n"
           "  port    TCP bootstrap 与 UDP/KCP 共用的监听端口（默认 9400）\n"
           "\n"
           "环境变量:\n"
           "  ORK_LOG_FILE   内部日志文件（默认 %s）\n"
           "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
           prog, ORK_DEVNULL);
}

int main(int argc, char** argv)
{
    uint16_t port = 9400;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        const long p = strtol(argv[i], NULL, 10);
        if (p <= 0 || p > 65535)
        {
            fprintf(stderr, "[host] 非法端口 \"%s\"\n\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
        port = (uint16_t)p;
    }

    printf(ORK_BANNER_RULE "\n");
    printf(" Demo 01 · C ABI echo server (pure C11)\n");
    printf(" C ABI version = %u (header expects %u)\n",
           ork_abi_version(), (unsigned)ORK_ABI_VERSION);
    printf(" sizeof(OrkHostConfig)=%zu  sizeof(OrkHostCallbacks)=%zu\n",
           sizeof(OrkHostConfig), sizeof(OrkHostCallbacks));
    printf(ORK_BANNER_RULE "\n");

    if (ork_abi_version() != ORK_ABI_VERSION)
    {
        fprintf(stderr, "[host] C ABI 版本不匹配，头文件与 libork_core.a 不同源\n");
        return 1;
    }

    /* ---- 1. 配置 ------------------------------------------------------- */
    OrkHostConfig cfg = ORK_HOST_CONFIG_INIT;   /* 已置 struct_size */
    cfg.tcp_port            = port;
    cfg.udp_port            = port;   /* TCP bootstrap 与 UDP/KCP 共用同一端口 */
    cfg.enable_kcp          = 1;      /* Reliable 通道走 KCP over UDP */
    cfg.enable_encryption   = 1;      /* AEAD（ChaCha20-Poly1305）；密钥由 bootstrap 协商 */
    cfg.bootstrap_ttl_sec   = 300;
    cfg.pump_rate_hz        = 30;     /* 宿主回调频率（与 server/config/network.yaml 对齐）*/
    cfg.kcp_update_rate_hz  = 100;    /* 必须 >= pump_rate_hz */
    cfg.idle_timeout_sec    = 300;
    cfg.max_packet_size     = 1200;   /* 单包 payload 上限，同时作为 KCP MTU */
    cfg.outbound_queue_capacity = 0;  /* 0 = 用 SDK 默认 */
    cfg.inbound_queue_capacity  = 0;
    cfg.drive_mode          = 0;      /* 0 = Internal（内部 pump 线程）；1 会被 create 拒绝 */
    cfg.log_path            = ork_demo_env_log_file();
    cfg.log_level           = ork_demo_env_log_level("host");

    OrkHostCallbacks cb = ORK_HOST_CALLBACKS_INIT;
    cb.user             = NULL;       /* demo 用全局变量，故无需上下文指针 */
    cb.on_connected     = on_connected;
    cb.on_disconnected  = on_disconnected;
    cb.on_payload       = on_payload;
    cb.on_stats         = on_stats;

    /* ---- 2. create ----------------------------------------------------- */
    OrkError err = ork_host_create(&cfg, &cb, &g_host);
    if (err != OrkOk || g_host == NULL)
    {
        fprintf(stderr, "[host] ork_host_create 失败 code=%d detail=%s\n",
                (int)err, ork_last_error());
        return 1;
    }

    /* ---- 3. start ------------------------------------------------------ */
    err = ork_host_start(g_host);
    if (err != OrkOk)
    {
        fprintf(stderr, "[host] ork_host_start 失败 code=%d detail=%s\n",
                (int)err, ork_last_error());
        ork_host_destroy(g_host);
        g_host = NULL;
        return 1;
    }

    /* 重复 start 必须被拒绝 —— 顺手演示错误码语义 */
    if (ork_host_start(g_host) != OrkErr_AlreadyStarted)
        fprintf(stderr, "[host] 警告: 重复 start 未返回 OrkErr_AlreadyStarted\n");

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("[host] listening on 127.0.0.1:%u  (TCP bootstrap + UDP/KCP)\n", (unsigned)port);
    printf("[host] bootstrap security = PlaintextDevelopment (loopback only, 开发用途)\n");
    printf("[host] encryption = %s, kcp = %s, max_packet = %u B\n",
           cfg.enable_encryption ? "on" : "off",
           cfg.enable_kcp ? "on" : "off",
           cfg.max_packet_size);
    printf("[host] 按 Ctrl+C 退出\n");
    fflush(stdout);

    /* ---- 4. 主循环 ----------------------------------------------------- */
    const uint64_t t0 = ork_demo_now_ms();
    uint64_t last_report = t0;

    while (!g_stop)
    {
        /* Internal 驱动模式下 pump_once 只是「协作式让出」（约 1ms），
         * 真正的网络 pump 在 SDK 内部线程上跑。 */
        if (ork_host_pump_once(g_host) != OrkOk)
        {
            fprintf(stderr, "[host] ork_host_pump_once 失败: %s\n", ork_last_error());
            break;
        }

        const uint64_t t = ork_demo_now_ms();
        if (t - last_report >= 10000u)
        {
            last_report = t;
            print_status((double)(t - t0));
        }
    }

    /* ---- 5. 干净退出 --------------------------------------------------- */
    printf("\n[host] shutting down...\n");
    fflush(stdout);

    err = ork_host_stop(g_host);
    if (err != OrkOk)
        fprintf(stderr, "[host] ork_host_stop 返回 code=%d detail=%s\n",
                (int)err, ork_last_error());

    print_status((double)(ork_demo_now_ms() - t0));

    ork_host_destroy(g_host);
    g_host = NULL;
    ork_host_destroy(NULL);   /* NULL 安全：必须是无操作 */

    printf("[host] bye\n");
    return 0;
}
