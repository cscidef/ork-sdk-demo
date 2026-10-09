/**
 * @file main.c
 * @brief Demo 02 —— 纯 C11 的最小客户端（C ABI Client）
 *
 * 展示内容
 * --------
 * 只用 `<ork/sdk/c/ork_client_abi.h>` 走完 Ork 客户端的完整连接流程：
 *
 *   TCP connect → Bootstrap V2（109B opaque 帧，拿到 connectionId /
 *   sessionToken / c2sKey / s2cKey）→ 绑定 UDP + 建立 KCP 会话 →
 *   UDP Handshake V2（42B 认证帧）→ 收到 "ORK-UDP-ACK:handshake" → Connected
 *
 * 然后在 **Reliable(KCP)** 与 **Unreliable(UDP)** 两条通道各发一个 25B
 * `ORKBENCH1` 探针帧，校验服务端原样回显。
 *
 * Client 没有内部线程
 * -------------------
 * 与 Host（drive_mode=0 自带 pump 线程）不同，**Client 完全靠调用方驱动**：
 * 必须反复调用 `ork_client_pump_once`，所有异步回调（含 on_connected /
 * on_payload）都在这个调用栈里同步触发。这正好契合游戏主循环 / Unity
 * `MonoBehaviour.Update()` 的形态。
 *
 * OrkClientConfig v1 / v2
 * -----------------------
 * 结构体首字段 `struct_size` 是版本判别器：
 *   - v1 = 72 字节（`ORK_CLIENT_CONFIG_V1_SIZE`），没有 bootstrap 安全字段；
 *   - v2 = 96 字节（`sizeof(OrkClientConfig)`），新增
 *     `bootstrap_security_mode` + 4 个 TLS 字段。
 * `ork_client_create` 只接受这两个白名单值，其余一律 `OrkErr_InvalidArg`，
 * 这样旧二进制不会因为结构体长大而读到垃圾内存。
 *
 * 用法
 * ----
 *   ./02_echo_client                                # 连 127.0.0.1:9400
 *   ./02_echo_client --port 9500 --timeout 15
 *   ./02_echo_client --host 127.0.0.1 --count 5
 *
 * 退出码：0 = 两通道回显全部一致；1 = 失败 / 超时；2 = 参数错误。
 */

#include "ork/sdk/c/ork_client_abi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  define ORK_DEVNULL "NUL"
#else
#  include <time.h>
#  define ORK_DEVNULL "/dev/null"
#endif

/* 25B opaque 回显帧：magic(9) + marker(8) + 时间戳 ns(8, 小端) */
#define ECHO_FRAME_SIZE   25u
#define ECHO_MAGIC        "ORKBENCH1"
#define ECHO_MAGIC_LEN    9u
#define MARKER_RELIABLE   "ORKDEMO1"
#define MARKER_UNRELIABLE "ORKDEMO2"

/* Unreliable 走裸 UDP，回显可能丢；重发几次是正常做法 */
#define UDP_MAX_ATTEMPTS  8
#define UDP_RETRY_MS      150

/* ---- 回调置位的状态（驱动线程写，主循环读；同一线程，volatile 足够） ---- */
static OrkClientHandle   g_client         = NULL;
static volatile sig_atomic_t g_stop       = 0;
static volatile int      g_connected      = 0;
static volatile int      g_disconnected   = 0;
static volatile uint32_t g_conn_id        = 0;
static volatile uint32_t g_disc_reason    = 0;
static volatile uint64_t g_payloads       = 0;
static volatile int      g_reliable_echo  = 0;
static volatile int      g_unreliable_echo = 0;
static volatile int      g_first_send_ok  = 0;   /* on_connected 里第 1 轮探针发送成功 */

static uint8_t g_frame_reliable[ECHO_FRAME_SIZE];
static uint8_t g_frame_unreliable[ECHO_FRAME_SIZE];

/* -------------------------------------------------------------------------- */
static double now_ms(void)
{
#if defined(_WIN32)
    return (double)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static uint64_t now_ns(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64() * 1000000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void put_u64_le(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

static uint64_t get_u64_le(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | (uint64_t)p[i];
    return v;
}

/** 组装一个 25B ORKBENCH1 探针帧 */
static void build_frame(uint8_t* out, const char* marker)
{
    memcpy(out, ECHO_MAGIC, ECHO_MAGIC_LEN);
    memcpy(out + ECHO_MAGIC_LEN, marker, 8);
    put_u64_le(out + ECHO_MAGIC_LEN + 8u, now_ns());
}

static const char* state_name(OrkClientState s)
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

static const char* send_result_name(OrkSendResult r)
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

static const char* disconnect_reason_name(uint32_t reason)
{
    switch ((OrkDisconnectReason)reason)
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

static void hexdump(const uint8_t* d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) printf("%02x", d[i]);
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* -------------------------------------------------------------------------- */
/* 回调 —— 注意：全部在 ork_client_pump_once / ork_client_disconnect 的        */
/*          调用栈里同步触发（Client 无内部线程）。                            */
/*          禁止在这里调用 ork_client_destroy（会析构正在执行的调用栈）。       */
/* -------------------------------------------------------------------------- */
static void on_connected(void* user, uint32_t connection_id)
{
    (void)user;
    g_conn_id   = connection_id;
    g_connected = 1;
    printf("[client] connected, connectionId=%u\n", connection_id);
    fflush(stdout);

    /* 此刻 state 已经是 Connected、KCP 会话已建好，直接在回调里发送是安全的。 */
    build_frame(g_frame_reliable, MARKER_RELIABLE);
    const OrkSendResult r = ork_client_send(g_client, g_frame_reliable,
                                            ECHO_FRAME_SIZE, OrkChannel_Reliable);
    printf("[client] -> Reliable(KCP)   %u B  ts=%llu  send=%s\n",
           ECHO_FRAME_SIZE,
           (unsigned long long)get_u64_le(g_frame_reliable + ECHO_MAGIC_LEN + 8u),
           send_result_name(r));
    fflush(stdout);
    if (r == OrkSend_Ok) g_first_send_ok = 1;
}

static void on_disconnected(void* user, uint32_t reason)
{
    (void)user;
    g_disc_reason  = reason;
    g_disconnected = 1;
    printf("[client] disconnected, reason=%s(%u)\n",
           disconnect_reason_name(reason), reason);
    fflush(stdout);
}

static void on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)user;
    g_payloads++;

    const int reliable = (channel == (uint32_t)OrkChannel_Reliable);
    const uint8_t* expect = reliable ? g_frame_reliable : g_frame_unreliable;

    if (size != ECHO_FRAME_SIZE || memcmp(data, ECHO_MAGIC, ECHO_MAGIC_LEN) != 0)
    {
        printf("[client] <- %s %u B 非 ORKBENCH1 帧: ",
               reliable ? "Reliable" : "Unreliable", size);
        hexdump(data, size < 32 ? size : 32);
        printf("\n");
        fflush(stdout);
        return;
    }

    if (memcmp(data, expect, ECHO_FRAME_SIZE) == 0)
    {
        if (reliable) g_reliable_echo = 1; else g_unreliable_echo = 1;

        /* 顺便量一下往返时延（帧里带的是发送时刻的单调时钟 ns） */
        const uint64_t sent_ns = get_u64_le(data + ECHO_MAGIC_LEN + 8u);
        const long long rtt_us = (long long)((now_ns() - sent_ns) / 1000ull);
        printf("[client] <- %s echo OK (%u B, rtt~%lld us)\n",
               reliable ? "Reliable  " : "Unreliable", size, rtt_us);
    }
    else
    {
        printf("[client] <- %s echo MISMATCH\n   got: ",
               reliable ? "Reliable" : "Unreliable");
        hexdump(data, size);
        printf("\n   exp: ");
        hexdump(expect, ECHO_FRAME_SIZE);
        printf("\n");
    }
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
static void usage(const char* prog)
{
    printf("用法: %s [--host H] [--port P] [--count N] [--timeout SEC] [--help]\n"
           "  --host     服务端地址（默认 127.0.0.1，明文 bootstrap 只允许 loopback）\n"
           "  --port     TCP bootstrap 与 UDP/KCP 共用端口（默认 9400）\n"
           "  --count    重复发几轮 Reliable 探针（默认 1）\n"
           "  --timeout  整体超时秒数（默认 10）\n",
           prog);
}

int main(int argc, char** argv)
{
    const char* host    = "127.0.0.1";
    uint16_t    port    = 9400;
    int         count   = 1;
    int         timeout = 10;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if ((strcmp(a, "--help") == 0) || (strcmp(a, "-h") == 0)) { usage(argv[0]); return 0; }
        if (i + 1 >= argc) { fprintf(stderr, "[client] %s 缺少参数值\n", a); usage(argv[0]); return 2; }
        const char* v = argv[++i];
        if      (strcmp(a, "--host")    == 0) host = v;
        else if (strcmp(a, "--port")    == 0) port = (uint16_t)atoi(v);
        else if (strcmp(a, "--count")   == 0) count = atoi(v);
        else if (strcmp(a, "--timeout") == 0) timeout = atoi(v);
        else { fprintf(stderr, "[client] 未知参数 %s\n", a); usage(argv[0]); return 2; }
    }
    if (count < 1) count = 1;
    if (timeout < 1) timeout = 1;

    printf("=============================================================\n");
    printf(" Demo 02 · C ABI echo client (pure C11)\n");
    printf(" C ABI version = %u (header expects %u)\n",
           ork_abi_version(), (unsigned)ORK_ABI_VERSION);
    printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE);
    printf(" target = %s:%u\n", host, (unsigned)port);
    printf("=============================================================\n");

    /* ---- 1. 配置（v2，struct_size 由 INIT 宏填成 sizeof） -------------- */
    OrkClientConfig cfg = ORK_CLIENT_CONFIG_INIT;
    cfg.host              = host;      /* 借用语义：create 内部会拷贝，返回后 argv 可失效 */
    cfg.tcp_port          = port;
    cfg.udp_port          = port;      /* ★ 必须显式设成和 tcp_port 一样：
                                          留 0 会回落到默认 9401，而 demo
                                          服务端 TCP/UDP 共用 9400。 */
    cfg.enable_kcp        = 1;
    cfg.enable_encryption = 1;         /* 客户端意向；权威是服务端 BootstrapFrame.flags */
    cfg.max_packet_size   = 1200;      /* 合法范围 [50, 1414] */
    cfg.connect_timeout_ms = 5000;
    cfg.kcp_stream_mode   = 0;         /* 必须为 0（消息模式），非 0 会被拒绝 */

    /* demo 服务端跑在 plaintext_dev（loopback only）。生产请用
     * OrkBootstrapSecurity_Tls 并填 tls_ca_file / tls_expected_server_name。 */
    cfg.bootstrap_security_mode    = OrkBootstrapSecurity_PlaintextDev;
    cfg.tls_ca_file                = NULL;
    cfg.tls_expected_server_name   = NULL;
    cfg.tls_use_system_trust_store = 0;
    cfg.tls_handshake_timeout_ms   = 5000;

    OrkClientCallbacks cb = ORK_CLIENT_CALLBACKS_INIT;
    cb.user             = NULL;
    cb.on_connected     = on_connected;
    cb.on_disconnected  = on_disconnected;
    cb.on_payload       = on_payload;

    /* ---- 2. create ----------------------------------------------------- */
    OrkError err = ork_client_create(&cfg, &cb, &g_client);
    if (err != OrkOk || g_client == NULL)
    {
        fprintf(stderr, "[client] ork_client_create 失败 code=%d detail=%s\n",
                (int)err, ork_client_last_error());
        return 1;
    }
    printf("[client] created, state=%s (期望 Disconnected)\n",
           state_name(ork_client_get_state(g_client)));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ---- 3. connect ---------------------------------------------------- */
    err = ork_client_connect(g_client);
    if (err != OrkOk)
    {
        fprintf(stderr, "[client] ork_client_connect 失败 code=%d detail=%s\n",
                (int)err, ork_client_last_error());
        ork_client_destroy(g_client);
        return 1;
    }

    /* ---- 4. 手动驱动 pump，直到两通道回显都到位或超时 ------------------ */
    const double t0 = now_ms();
    const double deadline = t0 + (double)timeout * 1000.0;

    int reliable_sent   = 0;
    int reliable_rounds = 0;
    int udp_attempts    = 0;
    double next_udp_try = 0.0;
    OrkClientState last_state = OrkClientState_Disconnected;
    while (!g_stop)
    {
        /* ★ Client 没有内部线程：不调 pump_once 就什么都不会发生。 */
        if (ork_client_pump_once(g_client) != OrkOk)
        {
            fprintf(stderr, "[client] pump_once 失败: %s\n", ork_client_last_error());
            break;
        }

        const double t = now_ms();

        /* 状态机变化打点，方便看清 bootstrap / handshake 各阶段 */
        const OrkClientState st = ork_client_get_state(g_client);
        if (st != last_state)
        {
            printf("[client] state: %s -> %s\n", state_name(last_state), state_name(st));
            fflush(stdout);
            last_state = st;
        }

        if (g_connected)
        {
            /* on_connected 回调里已经发了第 1 轮 Reliable 探针，计作 round 1 */
            if (!reliable_sent && g_first_send_ok) { reliable_sent = 1; reliable_rounds = 1; }

            /* Unreliable 通道：连接就绪后周期性重发，直到收到回显或用尽次数 */
            if (!g_unreliable_echo && udp_attempts < UDP_MAX_ATTEMPTS && t >= next_udp_try)
            {
                build_frame(g_frame_unreliable, MARKER_UNRELIABLE);
                const OrkSendResult r = ork_client_send(g_client, g_frame_unreliable,
                                                        ECHO_FRAME_SIZE,
                                                        OrkChannel_Unreliable);
                udp_attempts++;
                next_udp_try = t + UDP_RETRY_MS;
                printf("[client] -> Unreliable(UDP) %u B  attempt=%d  send=%s\n",
                       ECHO_FRAME_SIZE, udp_attempts, send_result_name(r));
                fflush(stdout);
            }

            /* 后续 Reliable 轮次：上一轮回显到手才发下一轮（天然限流） */
            if (g_reliable_echo && reliable_rounds < count)
            {
                build_frame(g_frame_reliable, MARKER_RELIABLE);
                const OrkSendResult r = ork_client_send(g_client, g_frame_reliable,
                                                        ECHO_FRAME_SIZE,
                                                        OrkChannel_Reliable);
                if (r == OrkSend_Ok) reliable_rounds++;
                g_reliable_echo = 0;   /* 等这一轮的回显 */
                printf("[client] -> Reliable(KCP)   %u B  round=%d  send=%s\n",
                       ECHO_FRAME_SIZE, reliable_rounds, send_result_name(r));
                fflush(stdout);
            }

            /* 收敛：最后一轮 Reliable 回显到手 + Unreliable 有结果 */
            if (reliable_rounds >= count && g_reliable_echo
                && (g_unreliable_echo || udp_attempts >= UDP_MAX_ATTEMPTS))
                break;
        }

        if (g_disconnected) break;
        if (t >= deadline) { fprintf(stderr, "[client] 超时（%d s）\n", timeout); break; }

        sleep_ms(1);
    }

    const int reliable_ok = reliable_sent && (reliable_rounds >= count) && g_reliable_echo;
    const int unreliable_ok = g_unreliable_echo != 0;

    /* ---- 5. 断开 + 销毁 ------------------------------------------------ */
    if (!g_disconnected)
    {
        (void)ork_client_disconnect(g_client);
        const double disc_deadline = now_ms() + 2000.0;
        while (!g_disconnected && now_ms() < disc_deadline && !g_stop)
        {
            (void)ork_client_pump_once(g_client);
            sleep_ms(1);
        }
    }

    printf("[client] final state=%s  connectionId=%u  payloads=%llu\n",
           state_name(ork_client_get_state(g_client)),
           ork_client_get_connection_id(g_client),
           (unsigned long long)g_payloads);

    ork_client_destroy(g_client);
    g_client = NULL;
    ork_client_destroy(NULL);   /* NULL 安全 */

    /* ---- 6. 结论 ------------------------------------------------------- */
    printf("-------------------------------------------------------------\n");
    printf(" Reliable(KCP)   echo : %s\n", reliable_ok ? "OK" : "FAIL");
    printf(" Unreliable(UDP) echo : %s (%d attempts)\n",
           unreliable_ok ? "OK" : "FAIL", udp_attempts);
    printf("-------------------------------------------------------------\n");

    const int pass = g_connected && reliable_ok && unreliable_ok && !g_stop;
    printf("%s\n", pass ? "PASS: C ABI echo roundtrip" : "FAIL: C ABI echo roundtrip");
    return pass ? 0 : 1;
}
