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
 * `ORKBENCH1` 探针帧，校验服务端原样回显。两通道的 pump / 重试 / 收敛
 * 逻辑封装在 demo_support_c.h 的 `ork_demo_run_dual_channel_probe()` 里，
 * 与 C++ 侧（demo 04 / 06、test_cpp_roundtrip）的 `RunDualChannelProbe`
 * 是同一套语义。
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

/* demo_support_c.h 已经带进 ork_abi.h / ork_client_abi.h、平台差异宏、
 * 25B 探针帧协议常量与全部共享小工具。 */
#include "demo_support_c.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* 跨线程 / 跨回调共享状态                                                     */
/*                                                                            */
/* Client 没有内部线程，回调都在 pump_once 的调用栈里同步触发，所以这些量      */
/* 实际上是单线程访问的。仍然用 _Atomic（而不是 volatile）—— volatile 在 C11   */
/* 里不构成任何同步保证，_Atomic 才是；换到有内部线程的 Host（demo 01）也       */
/* 立刻就是正确的。                                                            */
/* -------------------------------------------------------------------------- */
static OrkClientHandle        g_client       = NULL;  /* 回调里也要用，故置全局 */
static _Atomic sig_atomic_t   g_stop         = 0;
static _Atomic uint32_t       g_disc_reason  = 0;
static _Atomic uint64_t       g_payloads     = 0;

/** 探针驱动读写的连接状态；由下面三个回调维护 */
static OrkDemoProbeState      g_probe        = { 0, 0, 0, 0, 0 };

static uint8_t g_frame_reliable[ORK_ECHO_FRAME_SIZE];
static uint8_t g_frame_unreliable[ORK_ECHO_FRAME_SIZE];

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* -------------------------------------------------------------------------- */
/* 回调 —— 注意：全部在 ork_client_pump_once / ork_client_disconnect 的        */
/*          调用栈里同步触发（Client 无内部线程）。                            */
/*          禁止在这里调用 ork_client_destroy（会析构正在执行的调用栈）。       */
/* -------------------------------------------------------------------------- */
static void on_connected(void* user, uint32_t connection_id)
{
    (void)user;
    g_probe.connected = 1;
    printf("[client] connected, connectionId=%u\n", connection_id);
    fflush(stdout);

    /* 此刻 state 已经是 Connected、KCP 会话已建好，直接在回调里发送是安全的。
     * 这一轮算 round 1，所以探针驱动的 first_round_sent_in_callback = 1。 */
    ork_demo_build_frame(g_frame_reliable, ORK_MARKER_RELIABLE);
    const OrkSendResult r = ork_client_send(g_client, g_frame_reliable,
                                            ORK_ECHO_FRAME_SIZE, OrkChannel_Reliable);
    printf("[client] -> Reliable(KCP)   %u B  ts=%llu  send=%s\n",
           ORK_ECHO_FRAME_SIZE,
           (unsigned long long)ork_demo_get_u64_le(
               g_frame_reliable + ORK_ECHO_MAGIC_LEN + ORK_MARKER_LEN),
           ork_demo_send_result_name(r));
    fflush(stdout);
    if (r == OrkSend_Ok) g_probe.first_send_ok = 1;
}

static void on_disconnected(void* user, uint32_t reason)
{
    (void)user;
    g_disc_reason     = reason;
    g_probe.disconnected = 1;
    printf("[client] disconnected, reason=%s(%u)\n",
           ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason);
    fflush(stdout);
}

static void on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)user;
    g_payloads++;

    const int reliable = (channel == (uint32_t)OrkChannel_Reliable);
    const uint8_t* expect = reliable ? g_frame_reliable : g_frame_unreliable;

    if (size != ORK_ECHO_FRAME_SIZE || !ork_demo_is_echo_magic(data, size))
    {
        printf("[client] <- %s %u B 非 ORKBENCH1 帧: ",
               reliable ? "Reliable" : "Unreliable", size);
        ork_demo_hexdump(data, size < 32 ? size : 32);
        printf("\n");
        fflush(stdout);
        return;
    }

    if (memcmp(data, expect, ORK_ECHO_FRAME_SIZE) == 0)
    {
        if (reliable) g_probe.reliable_echo = 1; else g_probe.unreliable_echo = 1;

        /* 顺便量一下往返时延（帧里带的是发送时刻的单调时钟 ns） */
        const uint64_t sent_ns =
            ork_demo_get_u64_le(data + ORK_ECHO_MAGIC_LEN + ORK_MARKER_LEN);
        const long long rtt_us = (long long)((ork_demo_now_ns() - sent_ns) / 1000ull);
        printf("[client] <- %s echo OK (%u B, rtt~%lld us)\n",
               reliable ? "Reliable  " : "Unreliable", size, rtt_us);
    }
    else
    {
        printf("[client] <- %s echo MISMATCH\n   got: ",
               reliable ? "Reliable" : "Unreliable");
        ork_demo_hexdump(data, size);
        printf("\n   exp: ");
        ork_demo_hexdump(expect, ORK_ECHO_FRAME_SIZE);
        printf("\n");
    }
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/* 双通道探针驱动（demo_support_c.h）需要的适配层                              */
/* -------------------------------------------------------------------------- */
static OrkError probe_pump(void* ctx)
{
    /* ★ Client 没有内部线程：不调 pump_once 就什么都不会发生。 */
    const OrkError e = ork_client_pump_once((OrkClientHandle)ctx);
    if (e != OrkOk)
        fprintf(stderr, "[client] pump_once 失败: %s\n", ork_client_last_error());
    return e;
}

static OrkSendResult probe_send(void* ctx, uint8_t* frame, uint32_t size,
                                OrkChannel channel)
{
    /* 每次发送前重新盖时间戳，RTT 才量的是这一轮 */
    ork_demo_build_frame(frame, channel == OrkChannel_Reliable ? ORK_MARKER_RELIABLE
                                                              : ORK_MARKER_UNRELIABLE);
    return ork_client_send((OrkClientHandle)ctx, frame, size, channel);
}

static OrkClientState probe_get_state(void* ctx)
{
    return ork_client_get_state((OrkClientHandle)ctx);
}

static void probe_on_state_change(void* ctx, OrkClientState prev, OrkClientState cur)
{
    (void)ctx;
    /* 状态机变化打点，方便看清 bootstrap / handshake 各阶段 */
    printf("[client] state: %s -> %s\n",
           ork_demo_client_state_name(prev), ork_demo_client_state_name(cur));
    fflush(stdout);
}

static int probe_should_stop(void* ctx) { (void)ctx; return g_stop != 0; }

static void probe_log_send(void* ctx, OrkChannel channel, int index, OrkSendResult r)
{
    (void)ctx;
    if (channel == OrkChannel_Reliable)
        printf("[client] -> Reliable(KCP)   %u B  round=%d  send=%s\n",
               ORK_ECHO_FRAME_SIZE, index, ork_demo_send_result_name(r));
    else
        printf("[client] -> Unreliable(UDP) %u B  attempt=%d  send=%s\n",
               ORK_ECHO_FRAME_SIZE, index, ork_demo_send_result_name(r));
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

/**
 * @brief strtol + 严格范围校验
 * @param[out] rc  0 = 解析成功；2 = 非法（调用方应打印 usage 后 return 2）
 */
static int parse_int_arg(const char* what, const char* v, int lo, int hi, int* rc)
{
    char* end = NULL;
    const long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < (long)lo || n > (long)hi)
    {
        fprintf(stderr, "[client] %s 非法: \"%s\"（允许 %d..%d）\n", what, v, lo, hi);
        *rc = 2;
        return lo;
    }
    *rc = 0;
    return (int)n;
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
        int rc = 0;
        if      (strcmp(a, "--host")    == 0) host = v;
        else if (strcmp(a, "--port")    == 0) port = (uint16_t)parse_int_arg("--port", v, 1, 65535, &rc);
        else if (strcmp(a, "--count")   == 0) count = parse_int_arg("--count", v, 1, 1000000, &rc);
        else if (strcmp(a, "--timeout") == 0) timeout = parse_int_arg("--timeout", v, 1, 3600, &rc);
        else { fprintf(stderr, "[client] 未知参数 %s\n", a); usage(argv[0]); return 2; }
        if (rc != 0) { usage(argv[0]); return rc; }
    }

    printf(ORK_BANNER_RULE "\n");
    printf(" Demo 02 · C ABI echo client (pure C11)\n");
    printf(" C ABI version = %u (header expects %u)\n",
           ork_abi_version(), (unsigned)ORK_ABI_VERSION);
    printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE);
    printf(" target = %s:%u\n", host, (unsigned)port);
    printf(ORK_BANNER_RULE "\n");

    /* 头文件与 libork_core.a 不同源时，struct_size 白名单会误判 —— 早退。 */
    if (ork_abi_version() != ORK_ABI_VERSION)
    {
        fprintf(stderr, "[client] C ABI 版本不匹配，头文件与 libork_core.a 不同源\n");
        return 1;
    }

    /* ---- 1. 配置（v2，不变式见 demo_support_c.h 的 ork_demo_fill_client_config） */
    OrkClientConfig cfg;
    ork_demo_fill_client_config(&cfg, port, /*enable_encryption=*/1,
                                /*connect_timeout_ms=*/5000);
    cfg.host = host;   /* 借用语义：create 内部会拷贝，返回后 argv 可失效 */

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
           ork_demo_client_state_name(ork_client_get_state(g_client)));

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
    const OrkDemoProbeDriver driver = {
        .ctx              = g_client,
        .state            = &g_probe,
        .frame_reliable   = g_frame_reliable,
        .frame_unreliable = g_frame_unreliable,
        .pump             = probe_pump,
        .send             = probe_send,
        .get_state        = probe_get_state,
        .on_state_change  = probe_on_state_change,
        .should_stop      = probe_should_stop,
        .log_send         = probe_log_send,
    };

    OrkDemoProbeParams params = ork_demo_probe_params_default(count, timeout * 1000);
    params.pump_sleep_ms             = 1;   /* Client 靠调用方驱动，1ms 一拍足够灵敏 */
    params.first_round_sent_in_callback = 1; /* round 1 已在 on_connected 里发出 */

    const OrkDemoProbeResult probe = ork_demo_run_dual_channel_probe(&driver, &params);
    if (probe.timed_out) fprintf(stderr, "[client] 超时（%d s）\n", timeout);

    const int reliable_ok   = probe.reliable_ok;
    const int unreliable_ok = probe.unreliable_ok;

    /* ---- 5. 断开 + 销毁 ------------------------------------------------ */
    if (!g_probe.disconnected)
    {
        (void)ork_client_disconnect(g_client);
        const uint64_t disc_deadline = ork_demo_now_ms() + 2000u;
        while (!g_probe.disconnected && ork_demo_now_ms() < disc_deadline && !g_stop)
        {
            (void)ork_client_pump_once(g_client);
            ork_demo_sleep_ms(1);
        }
    }

    printf("[client] final state=%s  connectionId=%u  payloads=%llu\n",
           ork_demo_client_state_name(ork_client_get_state(g_client)),
           ork_client_get_connection_id(g_client),
           (unsigned long long)g_payloads);

    ork_client_destroy(g_client);
    g_client = NULL;
    ork_client_destroy(NULL);   /* NULL 安全 */

    /* ---- 6. 结论 ------------------------------------------------------- */
    printf("-------------------------------------------------------------\n");
    printf(" Reliable(KCP)   echo : %s\n", reliable_ok ? "OK" : "FAIL");
    printf(" Unreliable(UDP) echo : %s (%d attempts)\n",
           unreliable_ok ? "OK" : "FAIL", probe.udp_attempts);
    printf("-------------------------------------------------------------\n");

    /* 用全局 g_stop 而不是 probe.stopped：Ctrl+C 也可能落在探针之后的收尾阶段 */
    const int pass = g_probe.connected && reliable_ok && unreliable_ok && !g_stop;
    printf("%s\n", pass ? "PASS: C ABI echo roundtrip" : "FAIL: C ABI echo roundtrip");
    return pass ? 0 : 1;
}
