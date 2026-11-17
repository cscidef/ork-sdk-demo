/**
 * @file main.c
 * @brief Demo 05 —— C ABI 广播与扇出（全员广播 + 宿主按连接 ID 筛选）
 *
 * 教学目标
 * --------
 * C ABI 只有两个发送原语，本 demo 把它们的用法和三方语义讲透：
 *
 *   1. `ork_host_broadcast(host, data, size, channel)`
 *      —— 向**当前所有活跃连接**扇出一份相同字节。宿主不需要知道谁在线。
 *
 *   2. `ork_host_send(host, connection_id, data, size, channel)`
 *      —— 向**指定连接**发送。所谓"只发给其中一部分"，在 C ABI 里就是
 *         宿主自己维护一份 connection_id 表，遍历它、跳过不想发的那几个，
 *         然后逐条 `ork_host_send`。
 *
 * ★ C ABI **没有** C++ `ConnectionSet::Where/Of` 那种谓词集合 API，也
 *   **没有** `BroadcastReport` 那种逐项计数回执：`broadcast` 与 `send` 都只
 *   返回一个 `OrkSendResult`。所以"筛选广播"的代价（发了几条、几条成功）
 *   必须由宿主自己统计 —— 本 demo 就把它打成可见的计数器。
 *
 * ★ C ABI **没有**枚举连接列表的函数。
 *   `ork_host_connection_count()` 只给数量，`ork_host_get_connection_info()`
 *   需要已知 connection_id。因此宿主必须在 `on_connected` / `on_disconnected`
 *   回调里自己维护连接表 —— 这正是本 demo 的核心示范。
 *
 * 扇出矩阵（本 demo 的断言口径）
 * -------------------------------
 * 进程内起 N 个 C ABI 客户端。每个客户端连上后发一帧探针（marker=ORKBCSTP），
 * 宿主收到探针后做两件事：
 *
 *   (a) 全员广播  marker=ORKBCSTA  →  **每个**客户端（含探针来源）收到
 *   (b) 筛选发送  marker=ORKBCSTF  →  遍历连接表，**跳过来源**，逐个
 *                                     ork_host_send，于是除来源外各收一条
 *
 * N 个客户端各触发一轮，共 N 轮，于是每个客户端应收：
 *
 *        All      = N        （每轮 1 条 × N 轮）
 *        Filtered = N - 1    （其余 N-1 轮里它都不是来源）
 *
 * 打印成 N 行 × 2 列的矩阵，对的打 OK，不对的标出差异 —— 这就是本 demo
 * 独有的价值：把"扇出到底发给了谁"变成可断言的整数，而不是读文档猜。
 *
 * 防广播风暴
 * ----------
 * 只有 marker=ORKBCSTP 的**探针帧**才会触发放大；广播出去的 ORKBCSTA /
 * ORKBCSTF 回到宿主时被 `on_payload` 的 marker 门禁挡掉，否则会无限递归。
 *
 * 线程与生命周期
 * --------------
 *   - Host 回调跑在 SDK 内部 pump 线程；主线程只读计数 → 用 _Atomic。
 *   - 连接表的插入/删除只发生在 pump 线程内部（on_connected/on_disconnected
 *     不会并发），主线程读取用 atomic_load 拿一致快照。
 *   - `ork_host_broadcast` / `ork_host_send` 在回调里调用安全（只入队）。
 *   - `ork_host_stop` / `ork_host_destroy` 不能放回调里（自死锁），本 demo
 *     在主线程收尾。
 *
 * 依赖边界
 * --------
 * 只 include `demo_support_c.h` + C ABI 两枚头，无任何 C++ 头或内部 SDK 类型。
 *
 * 用法
 * ----
 *   ./05_broadcast                 # 进程内起 3 个客户端自证
 *   ./05_broadcast --clients 5
 *   ./05_broadcast --no-encryption
 *   ./05_broadcast --clients 0 --port 9500   # 纯服务端模式，Ctrl+C 退出
 *
 * 退出码：0 = 矩阵全部符合期望；1 = 不符 / 超时；2 = 参数错误。
 */

#include "demo_support_c.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* 三种 8B marker —— 前 17 字节（9B magic + 8B marker）用于分类               */
/* ========================================================================== */
#define ORKBC_PROBE    "ORKBCSTP"   /* client → host 探针，唯一触发放大的帧 */
#define ORKBC_ALL      "ORKBCSTA"   /* host → 全员广播帧 */
#define ORKBC_FILTERED "ORKBCSTF"   /* host → 筛选发送帧 */

#define ORKBC_MAX_CONN 32

/* ========================================================================== */
/* Host 侧上下文                                                               */
/* ========================================================================== */
typedef struct HostCtx
{
    OrkHost host;

    /* 连接表：槽位 0 表示空（connection_id 由 SDK 分配，不会为 0）。
     * 结构变更只在 pump 线程（回调）里发生；主线程只读快照。 */
    _Atomic uint32_t ids[ORKBC_MAX_CONN];
    _Atomic int      conn_n;

    _Atomic uint64_t probes;             /* 收到的探针帧 */
    _Atomic uint64_t rounds;             /* 实际放大轮数 */
    _Atomic uint64_t bcast_calls;        /* ork_host_broadcast 调用次数 */
    _Atomic uint64_t bcast_ok;           /* 返回值 Ok 的次数 */
    _Atomic uint64_t filtered_sends;     /* 筛选发送的 send 次数 */
    _Atomic uint64_t filtered_ok;        /* 其中 Ok 的次数 */
    _Atomic uint64_t filtered_fail;      /* 其中非 Ok 的次数 */
    _Atomic uint64_t disconnections;
} HostCtx;

/* ========================================================================== */
/* Client 侧上下文                                                             */
/* ========================================================================== */
typedef struct ClientCtx
{
    OrkClientHandle client;
    uint32_t        conn_id;
    int             connected;
    int             disconnected;
    int             error;

    uint32_t all_count;        /* 收到 ORKBCSTA 帧数 */
    uint32_t filtered_count;   /* 收到 ORKBCSTF 帧数 */
} ClientCtx;

/* 放大用的两帧模板（marker 固定，时间戳每次重盖） */
static uint8_t g_frame_all[ORK_ECHO_FRAME_SIZE];
static uint8_t g_frame_filtered[ORK_ECHO_FRAME_SIZE];

/* 全局停止标志（纯服务端模式下的 Ctrl+C） */
static _Atomic sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ========================================================================== */
/* 连接表维护（仅 pump 线程）                                                  */
/* ========================================================================== */
static void conn_table_add(HostCtx* ctx, uint32_t id)
{
    for (int i = 0; i < ORKBC_MAX_CONN; ++i)
    {
        uint32_t cur = atomic_load(&ctx->ids[i]);
        if (cur == 0)
        {
            atomic_store(&ctx->ids[i], id);
            atomic_fetch_add(&ctx->conn_n, 1);
            return;
        }
    }
    fprintf(stderr, "[host] ! 连接表已满 (%d)，connection %u 未纳入筛选广播\n",
            ORKBC_MAX_CONN, id);
}

static void conn_table_remove(HostCtx* ctx, uint32_t id)
{
    for (int i = 0; i < ORKBC_MAX_CONN; ++i)
    {
        if (atomic_load(&ctx->ids[i]) == id)
        {
            atomic_store(&ctx->ids[i], 0u);
            atomic_fetch_sub(&ctx->conn_n, 1);
            return;
        }
    }
}

/* ========================================================================== */
/* Host 回调（内部 pump 线程）                                                 */
/* ========================================================================== */
static void host_on_connected(void* user, uint32_t connection_id)
{
    HostCtx* ctx = (HostCtx*)user;
    conn_table_add(ctx, connection_id);
    printf("[host] + connection %u  (在线 %d)\n", connection_id, atomic_load(&ctx->conn_n));
    fflush(stdout);
}

static void host_on_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    HostCtx* ctx = (HostCtx*)user;
    conn_table_remove(ctx, connection_id);
    atomic_fetch_add(&ctx->disconnections, 1);
    printf("[host] - connection %u  reason=%s(%u)  (在线 %d)\n",
           connection_id, ork_demo_disconnect_reason_name((OrkDisconnectReason)reason),
           reason, atomic_load(&ctx->conn_n));
    fflush(stdout);
}

static void host_on_payload(void* user, uint32_t connection_id,
                            const uint8_t* data, uint32_t size, uint32_t channel)
{
    HostCtx* ctx = (HostCtx*)user;

    /* 门禁：只有探针帧才放大，否则广播回来的帧会自我递归 */
    if (size != ORK_ECHO_FRAME_SIZE || !ork_demo_is_echo_magic(data, size))
    {
        printf("[host] ? 非探针帧 %u B，忽略\n", size);
        fflush(stdout);
        return;
    }
    {
        const char* m = ork_demo_frame_marker(data, size);
        if (m == NULL || memcmp(m, ORKBC_PROBE, ORK_MARKER_LEN) != 0)
            return;   /* ORKBCSTA / ORKBCSTF 回流：不放大 */
    }

    atomic_fetch_add(&ctx->probes, 1);
    atomic_fetch_add(&ctx->rounds, 1);

    /* ---- (a) 全员广播：不关心谁在线 ----------------------------------- */
    ork_demo_build_frame(g_frame_all, ORKBC_ALL);
    atomic_fetch_add(&ctx->bcast_calls, 1);
    const OrkSendResult rb =
        ork_host_broadcast(ctx->host, g_frame_all, ORK_ECHO_FRAME_SIZE,
                           (OrkChannel)channel);
    if (rb == OrkSend_Ok) atomic_fetch_add(&ctx->bcast_ok, 1);
    printf("[host] broadcast(All)     %u B via %s -> %s  (在线 %d)\n",
           ORK_ECHO_FRAME_SIZE, ork_demo_channel_name((OrkChannel)channel),
           ork_demo_send_result_name(rb), atomic_load(&ctx->conn_n));

    /* ---- (b) 筛选发送：遍历自维护的连接表，跳过来源 -------------------- */
    ork_demo_build_frame(g_frame_filtered, ORKBC_FILTERED);
    int attempted = 0, ok = 0, fail = 0;
    const int n = atomic_load(&ctx->conn_n);
    for (int i = 0; i < ORKBC_MAX_CONN; ++i)
    {
        const uint32_t id = atomic_load(&ctx->ids[i]);
        if (id == 0 || id == connection_id) continue;   /* 空槽 / 跳过来源 */

        ++attempted;
        atomic_fetch_add(&ctx->filtered_sends, 1);
        const OrkSendResult rs =
            ork_host_send(ctx->host, id, g_frame_filtered, ORK_ECHO_FRAME_SIZE,
                          (OrkChannel)channel);
        if (rs == OrkSend_Ok) { ++ok; atomic_fetch_add(&ctx->filtered_ok, 1); }
        else                  { ++fail; atomic_fetch_add(&ctx->filtered_fail, 1); }
    }
    printf("[host] filtered(origin=%u) attempted=%d ok=%d fail=%d  (在线 %d)\n",
           connection_id, attempted, ok, fail, n);
    fflush(stdout);
}

/* ========================================================================== */
/* Client 回调（pump_once 调用栈内）                                           */
/* ========================================================================== */
static void client_on_connected(void* user, uint32_t connection_id)
{
    ClientCtx* ctx = (ClientCtx*)user;
    ctx->conn_id   = connection_id;
    ctx->connected = 1;
}

static void client_on_disconnected(void* user, uint32_t reason)
{
    ClientCtx* ctx = (ClientCtx*)user;
    ctx->disconnected = 1;
    printf("[client %u] disconnected reason=%s(%u)\n", ctx->conn_id,
           ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason);
    fflush(stdout);
}

static void client_on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)channel;
    ClientCtx* ctx = (ClientCtx*)user;

    if (size != ORK_ECHO_FRAME_SIZE || !ork_demo_is_echo_magic(data, size))
    {
        ctx->error = 1;
        return;
    }

    const char* m = ork_demo_frame_marker(data, size);
    if (m == NULL) { ctx->error = 1; return; }

    if (memcmp(m, ORKBC_ALL, ORK_MARKER_LEN) == 0)         ctx->all_count++;
    else if (memcmp(m, ORKBC_FILTERED, ORK_MARKER_LEN) == 0) ctx->filtered_count++;
    /* 其他 marker（如自己的探针回显）不计入矩阵 */
}

/* ========================================================================== */
/* 客户端探针发送                                                              */
/* ========================================================================== */
static void client_send_probe(ClientCtx* ctx)
{
    uint8_t frame[ORK_ECHO_FRAME_SIZE];
    ork_demo_build_frame(frame, ORKBC_PROBE);
    const OrkSendResult r =
        ork_client_send(ctx->client, frame, ORK_ECHO_FRAME_SIZE, OrkChannel_Reliable);
    printf("[client %u] -> probe send=%s\n", ctx->conn_id, ork_demo_send_result_name(r));
    fflush(stdout);
}

/* ========================================================================== */
/* 驱动所有客户端一拍                                                          */
/* ========================================================================== */
static void pump_all(ClientCtx* clients, int n)
{
    for (int i = 0; i < n; ++i)
        (void)ork_client_pump_once(clients[i].client);
}

/* ========================================================================== */
static void usage(const char* prog)
{
    printf("用法: %s [--clients N] [--port P] [--timeout SEC] [--no-encryption] [--help]\n"
           "  --clients N      进程内自证模式起几个客户端（默认 3；0 = 纯服务端模式）\n"
           "  --port P         纯服务端模式监听端口（默认 9400）\n"
           "  --timeout SEC    自证模式整体超时（默认 20）\n"
           "  --no-encryption  关掉 AEAD 对照（默认开）\n"
           "\n环境变量:\n"
           "  ORK_LOG_FILE   内部日志文件（默认 %s）\n"
           "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
           prog, ORK_DEVNULL);
}

int main(int argc, char** argv)
{
    int      n_clients  = 3;
    uint16_t port       = 9400;
    int      timeout_s  = 20;
    int      encryption = 1;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(argv[0]); return 0; }
        if (strcmp(a, "--no-encryption") == 0) { encryption = 0; continue; }
        if (i + 1 >= argc) { fprintf(stderr, "[demo] %s 缺少参数值\n", a); usage(argv[0]); return 2; }
        const char* v = argv[++i];
        if (strcmp(a, "--clients") == 0)
        {
            const long x = strtol(v, NULL, 10);
            if (x < 0 || x > ORKBC_MAX_CONN) { fprintf(stderr, "[demo] --clients 允许 0..%d\n", ORKBC_MAX_CONN); return 2; }
            n_clients = (int)x;
        }
        else if (strcmp(a, "--port") == 0)
        {
            const long x = strtol(v, NULL, 10);
            if (x <= 0 || x > 65535) { fprintf(stderr, "[demo] --port 允许 1..65535\n"); return 2; }
            port = (uint16_t)x;
        }
        else if (strcmp(a, "--timeout") == 0)
        {
            const long x = strtol(v, NULL, 10);
            if (x < 1 || x > 600) { fprintf(stderr, "[demo] --timeout 允许 1..600\n"); return 2; }
            timeout_s = (int)x;
        }
        else { fprintf(stderr, "[demo] 未知参数 %s\n", a); usage(argv[0]); return 2; }
    }

    printf("%s\n", ORK_BANNER_RULE);
    printf(" Demo 05 · C ABI 广播与扇出（纯 C11）\n");
    printf(" ORK_ABI_VERSION=%u  clients=%d  encryption=%s\n",
           ork_abi_version(), n_clients, encryption ? "on" : "off");
    printf(" 原语：ork_host_broadcast(全员) + ork_host_send(按 connection_id 逐个)\n");
    printf("%s\n", ORK_BANNER_RULE);
    fflush(stdout);

    if (ork_abi_version() != ORK_ABI_VERSION)
    {
        fprintf(stderr, "[demo] C ABI 版本不匹配，头文件与归档不同源\n");
        return 1;
    }

    /* ---- 1. Host -------------------------------------------------------- */
    HostCtx hctx;
    memset(&hctx, 0, sizeof(hctx));
    for (int i = 0; i < ORKBC_MAX_CONN; ++i) atomic_store(&hctx.ids[i], 0u);

    OrkHostConfig hcfg;
    ork_demo_fill_host_config(&hcfg, port, encryption);
    hcfg.log_path  = ork_demo_env_log_file();
    hcfg.log_level = ork_demo_env_log_level("demo");

    OrkHostCallbacks hcb = ORK_HOST_CALLBACKS_INIT;
    hcb.user            = &hctx;
    hcb.on_connected    = host_on_connected;
    hcb.on_disconnected = host_on_disconnected;
    hcb.on_payload      = host_on_payload;

    if (ork_host_create(&hcfg, &hcb, &hctx.host) != OrkOk || hctx.host == NULL)
    {
        fprintf(stderr, "[demo] ork_host_create 失败: %s\n", ork_last_error());
        return 1;
    }
    if (ork_host_start(hctx.host) != OrkOk)
    {
        fprintf(stderr, "[demo] ork_host_start 失败: %s\n", ork_last_error());
        ork_host_destroy(hctx.host);
        return 1;
    }
    printf("[host] listening 127.0.0.1:%u (TCP + UDP/KCP)\n", (unsigned)port);
    fflush(stdout);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ---- 2. 纯服务端模式：等外部客户端连进来，Ctrl+C 退出 -------------- */
    if (n_clients == 0)
    {
        printf("[host] 纯服务端模式：等外部客户端连接（探针帧 ORKBCSTP 触发放大），Ctrl+C 退出\n");
        fflush(stdout);
        while (!g_stop)
        {
            (void)ork_host_pump_once(hctx.host);
            ork_demo_sleep_ms(20);
        }
        printf("\n[host] 收到退出信号，probes=%llu rounds=%llu bcast(ok/calls)=%llu/%llu "
               "filtered(ok/fail)=%llu/%llu\n",
               (unsigned long long)atomic_load(&hctx.probes),
               (unsigned long long)atomic_load(&hctx.rounds),
               (unsigned long long)atomic_load(&hctx.bcast_ok),
               (unsigned long long)atomic_load(&hctx.bcast_calls),
               (unsigned long long)atomic_load(&hctx.filtered_ok),
               (unsigned long long)atomic_load(&hctx.filtered_fail));
        ork_host_stop(hctx.host);
        ork_host_destroy(hctx.host);
        return 0;
    }

    ClientCtx* clients = (ClientCtx*)calloc((size_t)n_clients, sizeof(ClientCtx));
    if (clients == NULL)
    {
        ork_host_stop(hctx.host);
        ork_host_destroy(hctx.host);
        return 1;
    }

    /* ---- 3. 建并连接 N 个客户端 ---------------------------------------- */
    for (int i = 0; i < n_clients; ++i)
    {
        ClientCtx* c = &clients[i];

        OrkClientConfig ccfg;
        ork_demo_fill_client_config(&ccfg, port, encryption, 5000);

        OrkClientCallbacks ccb = ORK_CLIENT_CALLBACKS_INIT;
        ccb.user            = c;
        ccb.on_connected    = client_on_connected;
        ccb.on_disconnected = client_on_disconnected;
        ccb.on_payload      = client_on_payload;

        if (ork_client_create(&ccfg, &ccb, &c->client) != OrkOk || c->client == NULL)
        {
            fprintf(stderr, "[demo] client %d create 失败: %s\n", i, ork_client_last_error());
            goto teardown;
        }
        if (ork_client_connect(c->client) != OrkOk)
        {
            fprintf(stderr, "[demo] client %d connect 失败: %s\n", i, ork_client_last_error());
            goto teardown;
        }
    }

    /* ---- 4. 等全部连上（此阶段不发探针，保证广播时连接表已满） --------- */
    {
        const uint64_t deadline = ork_demo_now_ms() + 10000u;
        for (;;)
        {
            if (g_stop) goto teardown;
            pump_all(clients, n_clients);

            int up = 0;
            for (int i = 0; i < n_clients; ++i) if (clients[i].connected) ++up;
            if (up == n_clients) break;
            if (ork_demo_now_ms() >= deadline)
            {
                fprintf(stderr, "[demo] 等待客户端连接超时（%d/%d 已连上）\n", up, n_clients);
                goto teardown;
            }
            ork_demo_sleep_ms(5);
        }
        printf("[demo] 全部 %d 个客户端已连接（host 在线 %d）\n",
               n_clients, atomic_load(&hctx.conn_n));
        fflush(stdout);
    }

    /* ---- 5. 每个客户端各发一帧探针，触发 N 轮放大 ---------------------- */
    for (int i = 0; i < n_clients; ++i)
    {
        client_send_probe(&clients[i]);
        pump_all(clients, n_clients);
        ork_demo_sleep_ms(20);
    }

    /* ---- 6. 等矩阵收敛 -------------------------------------------------- */
    {
        const uint64_t deadline = ork_demo_now_ms() + (uint64_t)timeout_s * 1000u;
        int settled = 0;
        for (;;)
        {
            if (g_stop) break;
            pump_all(clients, n_clients);

            int done = 1;
            for (int i = 0; i < n_clients; ++i)
            {
                if (clients[i].all_count < (uint32_t)n_clients
                    || clients[i].filtered_count < (uint32_t)(n_clients - 1))
                { done = 0; break; }
            }
            if (done) { settled = 1; break; }

            if (ork_demo_now_ms() >= deadline) break;
            ork_demo_sleep_ms(5);
        }

        /* ---- 7. 矩阵 ----------------------------------------------------- */
        printf("\n        client   All(N=%d)  Filtered(N-1=%d)   verdict\n",
               n_clients, n_clients - 1);
        int matrix_ok = 1;
        for (int i = 0; i < n_clients; ++i)
        {
            const int ok = (clients[i].all_count == (uint32_t)n_clients)
                        && (clients[i].filtered_count == (uint32_t)(n_clients - 1))
                        && !clients[i].error;
            if (!ok) matrix_ok = 0;
            printf("        #%-6d  %-9u %-16u  %s\n",
                   i, clients[i].all_count, clients[i].filtered_count,
                   ok ? "OK" : "MISMATCH");
        }

        printf("[host] 汇总: probes=%llu rounds=%llu broadcast(ok/calls)=%llu/%llu "
               "filtered(ok/fail/sends)=%llu/%llu/%llu  clients=%d settled=%s\n",
               (unsigned long long)atomic_load(&hctx.probes),
               (unsigned long long)atomic_load(&hctx.rounds),
               (unsigned long long)atomic_load(&hctx.bcast_ok),
               (unsigned long long)atomic_load(&hctx.bcast_calls),
               (unsigned long long)atomic_load(&hctx.filtered_ok),
               (unsigned long long)atomic_load(&hctx.filtered_fail),
               (unsigned long long)atomic_load(&hctx.filtered_sends),
               n_clients, settled ? "yes" : "no");

        const int pass = matrix_ok && settled
                      && atomic_load(&hctx.bcast_calls) == (uint64_t)n_clients
                      && atomic_load(&hctx.filtered_ok) == (uint64_t)(n_clients * (n_clients - 1));

        printf("-------------------------------------------------------------\n");
        printf("%s\n", pass ? "PASS: C ABI broadcast fanout (All + filtered per-connection send)"
                            : "FAIL: 扇出矩阵或服务端计数不符");

        /* ---- 8. 收尾 ----------------------------------------------------- */
        for (int i = 0; i < n_clients; ++i)
        {
            if (clients[i].client == NULL) continue;
            (void)ork_client_disconnect(clients[i].client);
        }
        {
            const uint64_t drain = ork_demo_now_ms() + 1500u;
            while (ork_demo_now_ms() < drain)
            {
                int all_down = 1;
                for (int i = 0; i < n_clients; ++i)
                    if (!clients[i].disconnected) { all_down = 0; break; }
                if (all_down) break;
                pump_all(clients, n_clients);
                ork_demo_sleep_ms(5);
            }
        }
        for (int i = 0; i < n_clients; ++i)
            if (clients[i].client != NULL) ork_client_destroy(clients[i].client);
        free(clients);
        clients = NULL;

        ork_host_stop(hctx.host);
        ork_host_destroy(hctx.host);
        return pass ? 0 : 1;
    }

teardown:
    if (clients != NULL)
    {
        for (int i = 0; i < n_clients; ++i)
            if (clients[i].client != NULL) ork_client_destroy(clients[i].client);
        free(clients);
    }
    ork_host_stop(hctx.host);
    ork_host_destroy(hctx.host);
    return 1;
}
