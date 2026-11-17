/**
 * @file main.c
 * @brief Demo 06 —— C ABI 加密：AEAD 密钥协商与透明收发（纯 C11）
 *
 * 展示内容
 * --------
 * 只调用 **C ABI**（`ork_host_*` + `ork_client_*`），在同一进程里跑两轮
 * host + client 配对，用可观测证据说明"加密是怎么打开的"：
 *
 *   Round A  enable_encryption = 1  →  OrkConnectionInfo.encryption_active == 1
 *   Round B  enable_encryption = 0  →  OrkConnectionInfo.encryption_active == 0（对照组）
 *
 * 两轮都做 Reliable(KCP) + Unreliable(UDP) 双通道 25B 回显，并断言：
 *   * 字节完全一致 —— 加解密对业务层 **完全透明**，payload 语义不变；
 *   * `OrkStats.crypto_fail_aead/replay/expired/malformed` 全为 0；
 *   * Round A 的 `encryption_active` 必须为 1，Round B 必须为 0。
 *
 * 密钥从哪来（这就是"bootstrap 期间协商 cipher"的含义）
 * ------------------------------------------------------
 * 客户端 TCP 连上后，服务端下发一帧 **Bootstrap V2（109B，opaque）**，里面带：
 *   connectionId / sessionToken / c2sKey / s2cKey / bootstrapEpoch / flags(含加密位)
 * 客户端据此建立 ChaCha20-Poly1305-IETF 的 OrkCipher 双向密钥；随后
 * **所有** UDP / KCP 数据帧都在传输层被逐帧 Seal / Open，业务层（本文件的
 * on_payload 回调）看到的永远是明文字节。密钥有效期由 `bootstrap_ttl_sec`
 * 控制（默认 300s），过期后 crypto_fail_expired 会开始计数。
 *
 * 服务端 `enable_encryption` 是 **权威**：客户端的同名字段只是"意向"，
 * 实际是否加密以服务端下发的 BootstrapFrame.flags 为准
 * （见 ork_client_abi.h 的 @note）。Round B 就是把服务端关掉做对照。
 *
 * ★ Bootstrap 通道本身的安全 ≠ 数据通道的加密。C ABI Host 恒定使用
 *   PlaintextDevelopment 模式（明文 TCP bootstrap），所以 SDK 会被强制
 *   bind 到 loopback，并在启动时打一条 critical 告警。
 *   生产环境请以 C++ SDK 接入（`SessionFramework` + TLS 1.3 证书），
 *   C ABI 目前没有 TLS bootstrap 的配置字段。
 *
 * 依赖边界
 * --------
 * 本文件只 include `demo_support_c.h`（C11 共享小工具：计时、25B 探针帧、
 * 临时端口、双通道探针驱动、枚举名）与 C ABI 两枚头文件，**不含任何 C++
 * 头或内部 SDK 类型**。链接仍走 C++ 驱动（静态包里是 C++ 实现），
 * 由 ork_configure_demo() 的 LINKER_LANGUAGE 处理。
 *
 * 用法
 * ----
 *   ./06_encryption                     # 跑 A/B 两轮
 *   ./06_encryption --timeout 20
 *   ./06_encryption --rounds a          # 只跑加密轮
 *   ORK_LOG_LEVEL=debug ORK_LOG_FILE=/tmp/ork.log ./06_encryption
 *
 * 退出码：0 = 两轮均符合预期；1 = 失败 / 超时；2 = 参数错误。
 */

#include "demo_support_c.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* 回调上下文                                                                  */
/*                                                                            */
/* Host 回调跑在 SDK 内部 pump 线程，主线程要读这些计数 → 一律 _Atomic。        */
/* Client 回调跑在 ork_client_pump_once / disconnect 的调用栈内（同线程），      */
/* 与主流程没有并发，用普通字段即可。                                           */
/* ========================================================================== */
typedef struct HostCtx
{
    OrkHost       host;                     /* 回调里也要用 */

    _Atomic uint32_t conn_id;
    _Atomic int      have_conn;
    _Atomic int      encryption_active;     /* -1=未知 0=off 1=on */
    _Atomic int      link_type;             /* -1=未知，否则 OrkLinkType */
    _Atomic uint64_t payloads;
    _Atomic uint64_t echoed;
    _Atomic uint64_t send_failures;
    _Atomic uint64_t disconnections;
    _Atomic uint64_t disconnects_by_reason[8];
    _Atomic uint64_t crypto_fails;
    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;
} HostCtx;

typedef struct ClientCtx
{
    int      connected;
    int      disconnected;
    uint32_t conn_id;
    uint32_t reason;
    uint64_t payloads;
    int      reliable_echo;
    int      unreliable_echo;
    int      mismatch;

    /* 双通道探针驱动读写的状态；回调里置位，驱动据此推进 */
    OrkDemoProbeState probe;

    /* 期望帧：主线程在 connect 之前填好；on_payload 在同一调用栈读取 */
    uint8_t frame_reliable[ORK_ECHO_FRAME_SIZE];
    uint8_t frame_unreliable[ORK_ECHO_FRAME_SIZE];
} ClientCtx;

/* ========================================================================== */
/* Host 回调（内部 pump 线程）                                                 */
/*                                                                            */
/* 线程契约：send / broadcast / disconnect 在回调里安全（只入队 / 异步 close）；*/
/* stop / destroy 绝对不行（要 join 当前线程 → 自死锁）。                      */
/* ========================================================================== */
static void host_on_connected(void* user, uint32_t connection_id)
{
    HostCtx* ctx = (HostCtx*)user;
    atomic_store(&ctx->conn_id, connection_id);
    atomic_store(&ctx->have_conn, 1);

    /* 这就是"cipher 已在 bootstrap 阶段协商完成"的服务端侧证据 */
    OrkConnectionInfo info = ORK_CONNECTION_INFO_INIT;
    if (ctx->host != NULL
        && ork_host_get_connection_info(ctx->host, connection_id, &info) == OrkOk)
    {
        atomic_store(&ctx->encryption_active, info.encryption_active ? 1 : 0);
        atomic_store(&ctx->link_type, (int)info.link);
        printf("[host] + connection %u  link=%s  encryption_active=%d  <<< bootstrap 协商结果\n",
               connection_id, ork_demo_link_name(info.link), (int)info.encryption_active);
    }
    else
    {
        printf("[host] + connection %u  (get_connection_info 失败)\n", connection_id);
    }
    fflush(stdout);
}

static void host_on_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    HostCtx* ctx = (HostCtx*)user;
    atomic_fetch_add(&ctx->disconnections, 1);
    if (reason < 8u) atomic_fetch_add(&ctx->disconnects_by_reason[reason], 1);
    printf("[host] - connection %u  reason=%s(%u)\n",
           connection_id, ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason);
    fflush(stdout);
}

static void host_on_payload(void* user, uint32_t connection_id,
                            const uint8_t* data, uint32_t size, uint32_t channel)
{
    HostCtx* ctx = (HostCtx*)user;
    atomic_fetch_add(&ctx->payloads, 1);

    /* 原样、同通道回显。data 的生命周期仅限本次回调。 */
    const OrkSendResult r =
        ork_host_send(ctx->host, connection_id, data, size, (OrkChannel)channel);
    if (r != OrkSend_Ok)
    {
        atomic_fetch_add(&ctx->send_failures, 1);
        printf("[host] ! echo to %u failed: %s\n",
               connection_id, ork_demo_send_result_name(r));
        fflush(stdout);
        return;
    }
    atomic_fetch_add(&ctx->echoed, 1);

    if (ork_demo_is_echo_magic(data, size))
    {
        char marker[ORK_MARKER_LEN + 1];
        memcpy(marker, ork_demo_frame_marker(data, size), ORK_MARKER_LEN);
        marker[ORK_MARKER_LEN] = '\0';
        printf("[host] ~ echo %u B to %u via %s (marker=%s)\n",
               size, connection_id, ork_demo_channel_name((OrkChannel)channel), marker);
    }
    else
    {
        printf("[host] ~ echo %u B to %u via %s\n",
               size, connection_id, ork_demo_channel_name((OrkChannel)channel));
    }
    fflush(stdout);
}

static void host_on_stats(void* user, const OrkStats* stats)
{
    if (stats == NULL) return;
    HostCtx* ctx = (HostCtx*)user;
    /* 加密失败四计数器：任何一项非 0 都说明 AEAD 协商/密钥出了问题 */
    atomic_store(&ctx->crypto_fails, ork_demo_crypto_fail_total(stats));
    atomic_store(&ctx->bytes_in, (uint64_t)stats->bytes_in);
    atomic_store(&ctx->bytes_out, (uint64_t)stats->bytes_out);
}

/* ========================================================================== */
/* Client 回调（pump_once / disconnect 调用栈内）                              */
/* ========================================================================== */
static void client_on_connected(void* user, uint32_t connection_id)
{
    ClientCtx* ctx = (ClientCtx*)user;
    ctx->conn_id   = connection_id;
    ctx->connected = 1;
    /* 探针驱动只在 probe.connected 为真时才推进发送逻辑 —— 必须置位 */
    ctx->probe.connected = 1;
    printf("[client] OnConnected connectionId=%u -> 立刻发两通道探针\n", connection_id);
    fflush(stdout);
}

static void client_on_disconnected(void* user, uint32_t reason)
{
    ClientCtx* ctx = (ClientCtx*)user;
    ctx->reason          = reason;
    ctx->disconnected    = 1;
    ctx->probe.disconnected = 1;
    printf("[client] OnDisconnected reason=%s(%u)\n",
           ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason);
    fflush(stdout);
}

static void client_on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    ClientCtx* ctx = (ClientCtx*)user;
    ctx->payloads++;

    const int reliable = (channel == (uint32_t)OrkChannel_Reliable);
    const uint8_t* expect = reliable ? ctx->frame_reliable : ctx->frame_unreliable;

    if (size != ORK_ECHO_FRAME_SIZE || !ork_demo_is_echo_magic(data, size))
    {
        ctx->mismatch = 1;
        fprintf(stderr, "[client] ! %s 收到非预期帧 (%u B)\n",
                reliable ? "Reliable" : "Unreliable", size);
        return;
    }

    if (memcmp(data, expect, ORK_ECHO_FRAME_SIZE) != 0)
    {
        ctx->mismatch = 1;
        fprintf(stderr, "[client] ! %s 回显字节不一致\n",
                reliable ? "Reliable" : "Unreliable");
        return;
    }

    if (reliable) { ctx->reliable_echo = 1;   ctx->probe.reliable_echo = 1; }
    else          { ctx->unreliable_echo = 1; ctx->probe.unreliable_echo = 1; }

    printf("[client] <- %s 回显逐字节一致 (%u B) —— 加解密对业务层透明\n",
           reliable ? "Reliable(KCP)  " : "Unreliable(UDP)", size);
    fflush(stdout);
}

/* ========================================================================== */
/* 双通道探针驱动（demo_support_c.h 的函数指针版）                             */
/* ========================================================================== */
static OrkError probe_pump(void* ctx)
{
    /* Client 没有内部线程：不调 pump_once 就一步都不会推进 */
    const OrkError e = ork_client_pump_once((OrkClientHandle)ctx);
    if (e != OrkOk)
        fprintf(stderr, "[client] pump_once 失败: %s\n", ork_client_last_error());
    return e;
}

static OrkSendResult probe_send(void* ctx, uint8_t* frame, uint32_t size, OrkChannel channel)
{
    /* 每次发送前重盖时间戳，RTT 才量的是这一轮 */
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
    printf("[client] state: %s -> %s\n",
           ork_demo_client_state_name(prev), ork_demo_client_state_name(cur));
    fflush(stdout);
}

static void probe_log_send(void* ctx, OrkChannel channel, int index, OrkSendResult r)
{
    (void)ctx;
    if (channel == OrkChannel_Reliable)
        printf("[client] -> Reliable(KCP)   %u B round=%d send=%s\n",
               ORK_ECHO_FRAME_SIZE, index, ork_demo_send_result_name(r));
    else
        printf("[client] -> Unreliable(UDP) %u B attempt=%d send=%s\n",
               ORK_ECHO_FRAME_SIZE, index, ork_demo_send_result_name(r));
    fflush(stdout);
}

/* ========================================================================== */
/* 单轮结果 / 拆除                                                             */
/* ========================================================================== */
typedef struct RoundResult
{
    int      ok;
    int      host_created;
    int      client_created;
    int      connected;
    int      reliable_echo;
    int      unreliable_echo;
    int      encryption_active;
    int      link_type;
    uint64_t crypto_fails;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint32_t host_conn_id;
    uint32_t client_conn_id;
    char     error[160];
} RoundResult;

static void destroy_host(OrkHost host)
{
    if (host == NULL) return;
    ork_host_stop(host);          /* 主线程调用，不能在 pump 线程里 */
    ork_host_destroy(host);
}

static void destroy_client(OrkClientHandle client, ClientCtx* cctx)
{
    if (client == NULL) return;
    (void)ork_client_disconnect(client);
    const uint64_t drain_until = ork_demo_now_ms() + 2000u;
    while (ork_demo_now_ms() < drain_until && !cctx->disconnected)
    {
        (void)ork_client_pump_once(client);
        ork_demo_sleep_ms(10);
    }
    ork_client_destroy(client);
}

/* ========================================================================== */
/* 建 host / client                                                            */
/* ========================================================================== */
static int create_host(uint16_t port, int encryption, HostCtx* hctx,
                       OrkHost* out_host, RoundResult* res)
{
    /* 全部不变式（tcp_port == udp_port、drive_mode == 0、
     * kcp_update_rate_hz >= pump_rate_hz、max_packet_size ∈ [50,1414] …）
     * 由 ork_demo_fill_host_config 统一给出，这里只覆盖日志字段。 */
    OrkHostConfig hcfg;
    ork_demo_fill_host_config(&hcfg, port, encryption);
    hcfg.log_path  = ork_demo_env_log_file();
    hcfg.log_level = ork_demo_env_log_level("demo");

    OrkHostCallbacks hcb = ORK_HOST_CALLBACKS_INIT;
    hcb.user            = hctx;
    hcb.on_connected    = host_on_connected;
    hcb.on_disconnected = host_on_disconnected;
    hcb.on_payload      = host_on_payload;
    hcb.on_stats        = host_on_stats;

    OrkHost host = NULL;
    OrkError e = ork_host_create(&hcfg, &hcb, &host);
    if (e != OrkOk || host == NULL)
    {
        snprintf(res->error, sizeof(res->error), "ork_host_create: %s", ork_last_error());
        fprintf(stderr, "[round] %s\n", res->error);
        return 0;
    }
    res->host_created = 1;
    hctx->host = host;            /* 回调里要用（get_connection_info / send） */

    e = ork_host_start(host);
    if (e != OrkOk)
    {
        snprintf(res->error, sizeof(res->error), "ork_host_start: %s", ork_last_error());
        fprintf(stderr, "[round] %s\n", res->error);
        destroy_host(host);
        return 0;
    }
    printf("[host] started  sizeof(OrkHostConfig)=%zu  sizeof(OrkHostCallbacks)=%zu\n",
           sizeof(OrkHostConfig), sizeof(OrkHostCallbacks));
    fflush(stdout);

    *out_host = host;
    return 1;
}

static int create_client(uint16_t port, int encryption, ClientCtx* cctx,
                         OrkClientHandle* out_client, RoundResult* res)
{
    OrkClientConfig ccfg;
    ork_demo_fill_client_config(&ccfg, port, encryption, /*connect_timeout_ms=*/5000);

    OrkClientCallbacks ccb = ORK_CLIENT_CALLBACKS_INIT;
    ccb.user            = cctx;
    ccb.on_connected    = client_on_connected;
    ccb.on_disconnected = client_on_disconnected;
    ccb.on_payload      = client_on_payload;

    OrkClientHandle client = NULL;
    OrkError e = ork_client_create(&ccfg, &ccb, &client);
    if (e != OrkOk || client == NULL)
    {
        snprintf(res->error, sizeof(res->error), "ork_client_create: %s",
                 ork_client_last_error());
        fprintf(stderr, "[round] %s\n", res->error);
        return 0;
    }
    res->client_created = 1;
    printf("[client] created  sizeof(OrkClientConfig)=%zu (v2)  "
           "ORK_CLIENT_CONFIG_V1_SIZE=%u  state=%s\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE,
           ork_demo_client_state_name(ork_client_get_state(client)));
    fflush(stdout);

    /* 期望帧（connect 之前就要就位：on_connected 之后紧接着就会收到回显） */
    ork_demo_build_frame(cctx->frame_reliable, ORK_MARKER_RELIABLE);
    ork_demo_build_frame(cctx->frame_unreliable, ORK_MARKER_UNRELIABLE);

    e = ork_client_connect(client);
    if (e != OrkOk)
    {
        snprintf(res->error, sizeof(res->error), "ork_client_connect: %s",
                 ork_client_last_error());
        fprintf(stderr, "[round] %s\n", res->error);
        ork_client_destroy(client);
        return 0;
    }

    *out_client = client;
    return 1;
}

/* ========================================================================== */
/* 收集证据                                                                    */
/* ========================================================================== */
static void collect_evidence(OrkClientHandle client, OrkHost host,
                             HostCtx* hctx, ClientCtx* cctx, RoundResult* res)
{
    /* 让 OnStats 至少落一次（~1Hz），否则 crypto_fail_* 恒为 0 没意义 */
    printf("[round] 等待统计快照 (~1.3s) ...\n");
    fflush(stdout);
    const uint64_t stats_until = ork_demo_now_ms() + 1300u;
    while (ork_demo_now_ms() < stats_until)
    {
        (void)ork_client_pump_once(client);
        ork_demo_sleep_ms(20);
    }

    OrkConnectionInfo info = ORK_CONNECTION_INFO_INIT;
    if (ork_host_get_connection_info(host, cctx->conn_id, &info) == OrkOk)
    {
        res->encryption_active = info.encryption_active ? 1 : 0;
        res->link_type         = (int)info.link;
    }
    else
    {
        /* 连接可能已经掉了，回退用回调里缓存的值 */
        res->encryption_active = atomic_load(&hctx->encryption_active);
        res->link_type         = atomic_load(&hctx->link_type);
    }

    OrkStats st = ORK_STATS_INIT;
    if (ork_host_get_stats(host, &st) == OrkOk)
    {
        res->crypto_fails = ork_demo_crypto_fail_total(&st);
        res->bytes_in     = (uint64_t)st.bytes_in;
        res->bytes_out    = (uint64_t)st.bytes_out;
        printf("[host] stats: aead=%llu replay=%llu expired=%llu malformed=%llu "
               "decode=%llu  bytes in/out=%llu/%llu  active=%u\n",
               (unsigned long long)st.crypto_fail_aead,
               (unsigned long long)st.crypto_fail_replay,
               (unsigned long long)st.crypto_fail_expired,
               (unsigned long long)st.crypto_fail_malformed,
               (unsigned long long)st.decode_failures,
               (unsigned long long)res->bytes_in,
               (unsigned long long)res->bytes_out,
               st.active_connections);
        fflush(stdout);
    }

    res->connected       = cctx->connected;
    res->reliable_echo   = cctx->reliable_echo;
    res->unreliable_echo = cctx->unreliable_echo;
    res->host_conn_id    = atomic_load(&hctx->conn_id);
    res->client_conn_id  = cctx->conn_id;
    if (cctx->mismatch) snprintf(res->error, sizeof(res->error), "回显字节不一致");
}

/* ========================================================================== */
/* 一轮 A/B 对照                                                               */
/* ========================================================================== */
static RoundResult run_round(const char* label, int encryption, int timeout_sec)
{
    RoundResult res;
    memset(&res, 0, sizeof(res));
    res.encryption_active = -1;
    res.link_type         = -1;

    printf("\n%s\n %s · enable_encryption = %d\n%s\n",
           ORK_BANNER_RULE, label, encryption, ORK_BANNER_RULE);
    fflush(stdout);

    /* ---- 0. 端口：TCP bootstrap 与 UDP/KCP 共用，必须两边都空闲 ---------- */
    const uint16_t port = ork_demo_pick_free_port();
    if (port == 0)
    {
        snprintf(res.error, sizeof(res.error), "pick_free_port 失败");
        fprintf(stderr, "[round] %s\n", res.error);
        return res;
    }
    printf("[round] loopback 端口 = %u (TCP + UDP 共用)\n", (unsigned)port);
    fflush(stdout);

    HostCtx  hctx;
    ClientCtx cctx;
    memset(&hctx, 0, sizeof(hctx));
    memset(&cctx, 0, sizeof(cctx));
    atomic_store(&hctx.encryption_active, -1);
    atomic_store(&hctx.link_type, -1);

    OrkHost         host   = NULL;
    OrkClientHandle client = NULL;

    if (!create_host(port, encryption, &hctx, &host, &res)) return res;
    if (!create_client(port, encryption, &cctx, &client, &res))
    {
        destroy_host(host);
        return res;
    }

    /* ---- 1. 手动驱动 + 双通道探针 --------------------------------------- */
    const OrkDemoProbeDriver driver = {
        .ctx              = client,
        .state            = &cctx.probe,
        .frame_reliable   = cctx.frame_reliable,
        .frame_unreliable = cctx.frame_unreliable,
        .pump             = probe_pump,
        .send             = probe_send,
        .get_state        = probe_get_state,
        .on_state_change  = probe_on_state_change,
        .should_stop      = NULL,
        .log_send         = probe_log_send,
    };

    OrkDemoProbeParams params = ork_demo_probe_params_default(1, timeout_sec * 1000);
    params.pump_sleep_ms = ORK_PUMP_SLEEP_MS;

    const OrkDemoProbeResult probe = ork_demo_run_dual_channel_probe(&driver, &params);
    if (probe.timed_out)
        fprintf(stderr, "[round] 双通道探针超时（%d s）\n", timeout_sec);

    /* 探针状态与 ClientCtx 是同一份（driver.state = &cctx.probe），
     * 这里只是把驱动视角的结论对齐到断言字段。 */
    cctx.connected       = cctx.probe.connected;
    cctx.reliable_echo   = cctx.probe.reliable_echo;
    cctx.unreliable_echo = cctx.probe.unreliable_echo;

    /* ---- 2. 证据 + 拆除 -------------------------------------------------- */
    collect_evidence(client, host, &hctx, &cctx, &res);
    destroy_client(client, &cctx);
    destroy_host(host);

    res.ok = res.connected && res.reliable_echo && res.unreliable_echo
          && res.crypto_fails == 0 && res.error[0] == '\0';
    return res;
}

static void print_row(const char* label, int expect_enc, const RoundResult* r)
{
    printf("  %-14s enc=%d  encryption_active=%d (期望 %d) %s  link=%s  "
           "connId=%u/%u  echo R/U=%s/%s  cryptoFail=%llu  bytes in/out=%llu/%llu  %s\n",
           label, expect_enc, r->encryption_active, expect_enc,
           r->encryption_active == expect_enc ? "OK" : "MISMATCH",
           r->link_type >= 0 ? ork_demo_link_name((OrkLinkType)r->link_type) : "?",
           r->host_conn_id, r->client_conn_id,
           r->reliable_echo ? "OK" : "FAIL",
           r->unreliable_echo ? "OK" : "FAIL",
           (unsigned long long)r->crypto_fails,
           (unsigned long long)r->bytes_in, (unsigned long long)r->bytes_out,
           r->ok ? "PASS" : "FAIL");
    fflush(stdout);
}

static void usage(const char* prog)
{
    printf("用法: %s [--timeout SEC] [--rounds a|b|both] [--help]\n"
           "  --timeout SEC  单轮超时（默认 15）\n"
           "  --rounds a     只跑加密轮（enable_encryption=1）\n"
           "  --rounds b     只跑对照轮（enable_encryption=0）\n"
           "  --rounds both  两轮都跑（默认）\n"
           "\n环境变量:\n"
           "  ORK_LOG_FILE   内部日志文件（默认 %s）\n"
           "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
           prog, ORK_DEVNULL);
}

int main(int argc, char** argv)
{
    int timeout_sec = 15;
    int run_a = 1, run_b = 1;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(argv[0]); return 0; }
        if (strcmp(a, "--timeout") == 0 && i + 1 < argc)
        {
            const long v = strtol(argv[++i], NULL, 10);
            if (v < 1 || v > 600) { fprintf(stderr, "[demo] --timeout 需在 [1,600]\n"); return 2; }
            timeout_sec = (int)v;
            continue;
        }
        if (strcmp(a, "--rounds") == 0 && i + 1 < argc)
        {
            const char* v = argv[++i];
            if      (strcmp(v, "a") == 0)    { run_a = 1; run_b = 0; }
            else if (strcmp(v, "b") == 0)    { run_a = 0; run_b = 1; }
            else if (strcmp(v, "both") == 0) { run_a = 1; run_b = 1; }
            else { fprintf(stderr, "[demo] --rounds 只接受 a|b|both\n"); return 2; }
            continue;
        }
        fprintf(stderr, "[demo] 未知参数 %s\n", a);
        usage(argv[0]);
        return 2;
    }

    printf("%s\n", ORK_BANNER_RULE);
    printf(" Demo 06 · C ABI 加密：AEAD 密钥协商与透明收发（纯 C11）\n");
    printf(" ORK_ABI_VERSION=%u  sizeof(OrkHostConfig)=%zu\n",
           ork_abi_version(), sizeof(OrkHostConfig));
    printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE);
    printf(" cipher = ChaCha20-Poly1305-IETF（密钥来自 Bootstrap V2 帧的 c2sKey/s2cKey）\n");
    printf("%s\n", ORK_BANNER_RULE);
    fflush(stdout);

    RoundResult a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    if (run_a) a = run_round("Round A (加密)", 1, timeout_sec);
    if (run_b) b = run_round("Round B (对照·明文)", 0, timeout_sec);

    printf("\n");
    ork_demo_print_banner("结果汇总");
    int ok = 1;
    if (run_a) { print_row("Round A 加密", 1, &a); ok = ok && a.ok && a.encryption_active == 1; }
    if (run_b) { print_row("Round B 对照", 0, &b); ok = ok && b.ok && b.encryption_active == 0; }

    printf("-------------------------------------------------------------\n");
    if (run_a && run_b)
    {
        printf(" 结论：encryption_active 随服务端 enable_encryption 翻转 (%d -> %d)，\n"
               "       两轮 payload 均逐字节一致，crypto_fail_* 全 0。\n"
               "       => AEAD 在 bootstrap 阶段协商完成，对业务层完全透明。\n",
               a.encryption_active, b.encryption_active);
    }
    printf("%s\n", ok ? "PASS: C ABI encryption negotiation"
                      : "FAIL: 见上面的 MISMATCH / FAIL 项");
    fflush(stdout);
    return ok ? 0 : 1;
}
