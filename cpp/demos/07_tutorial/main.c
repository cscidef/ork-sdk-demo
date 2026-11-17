/**
 * @file main.c
 * @brief Demo 07 —— C ABI 回调与生命周期教程（纯 C11，真实 Host + Client）
 *
 * 这个 demo 回答一个问题
 * ----------------------
 * **业务方要实现的回调长什么样，事件以什么顺序到达，我在里面能做什么。**
 *
 * 与 demo 01/02 的区别：01/02 演示"怎么把字节收发起来"，本 demo 用一条
 * 最小的**真实**连接（同进程 host + client，真的走 TCP bootstrap → UDP 握手
 * → KCP），把三个回调按顺序走一遍，每一步都停下来打印"Ork 此刻做了什么、
 * 业务方该做什么"。
 *
 * ★ 所有事件都是 Ork 真实产生的，不是 Mock 注入。想看纯接口签名而不跑网络
 *   的场景不存在于 C ABI —— 你无法在 C 侧替换 OrkHost 的实现，所以"模拟
 *   回调"只会教出错误的线程与生命周期预期。这里宁可真起一条连接。
 *
 * 三步教学流程
 * ------------
 *   Step 1  on_connected      —— TCP bootstrap 完成 + UDP/KCP 握手完成，
 *                                connectionId 被分配，此刻可以 send。
 *   Step 2  应用层命令 PING   —— 客户端在**字节里**写 "PING"；宿主 on_payload
 *                                读到后回 "PONG"。注意：Ork 完全不理解
 *                                "PING" 的含义，它只搬运 opaque 字节 ——
 *                                命令解析是业务方的责任，这是本 demo 想让你
 *                                记住的边界。
 *   Step 3  可靠 / 不可靠通道 —— 同一条连接上，Reliable 走 KCP（有序、可靠），
 *                                Unreliable 走裸 UDP（可能丢、不保序）。
 *                                用 ECHO: 命令各发一次，观察 channel 参数。
 *   Step 4  on_disconnected   —— 宿主用 ork_host_disconnect 主动踢掉连接
 *                                （reason=OrkDisconnect_Kicked），客户端收到
 *                                on_disconnected。这是"服务端关人"的标准做法。
 *
 * 线程模型（最容易踩坑的地方）
 * ----------------------------
 *   Host 回调   →  SDK 内部 pump 线程。可以 send / broadcast / disconnect；
 *                 绝不能 stop / destroy（会 join 自己 → 死锁）。
 *   Client 回调 →  ork_client_pump_once() / ork_client_disconnect() 的调用栈。
 *                 Client 没有内部线程，不 pump 就不动。
 *   main        →  收尾线程。stop / destroy 只在这里调用。
 *
 * 依赖边界：只 include demo_support_c.h + C ABI 两枚头，无 C++ / 内部类型。
 *
 * 用法
 * ----
 *   ./07_tutorial
 *   ./07_tutorial --timeout 15 --no-encryption
 *
 * 退出码：0 = 四步全部按预期观察到；1 = 失败 / 超时；2 = 参数错误。
 */

#include "demo_support_c.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* 应用层命令（纯业务语义，Ork 不认识这些字符串）                              */
/* ========================================================================== */
#define CMD_PING  "PING"
#define CMD_QUIT  "QUIT"
#define CMD_ECHO  "ECHO:"
#define RSP_PONG  "PONG"

#define LINE_MAX_LEN 128u

/* ========================================================================== */
/* Host 侧状态（回调在 SDK 内部 pump 线程）                                    */
/* ========================================================================== */
typedef struct HostState
{
    OrkHost   host;
    _Atomic uint32_t conn_id;
    _Atomic int      have_conn;
    _Atomic int      kicked;            /* 已主动踢人 */
    _Atomic uint64_t payloads;
    _Atomic uint64_t disconnections;
    _Atomic uint32_t disconnect_reason;
} HostState;

/* 主线程用来驱动 client；host 回调要读它做 send，故放在全局（单实例 demo） */
static OrkHost g_host = NULL;

/* ========================================================================== */
/* Client 侧状态（回调在 pump_once 调用栈，同线程）                            */
/* ========================================================================== */
typedef struct ClientState
{
    int      connected;
    int      disconnected;
    uint32_t conn_id;
    uint32_t reason;
    uint32_t payloads;

    int      got_pong;          /* 收到 "PONG" */
    int      got_echo_rel;      /* Reliable 通道的 ECHO 回包 */
    int      got_echo_unrel;    /* Unreliable 通道的 ECHO 回包 */
    uint32_t last_channel;      /* 最近一帧的通道 */
    char     last_text[LINE_MAX_LEN];
} ClientState;

/* ========================================================================== */
/* --------------------------------------------------------------------------
 * Step 1/4 —— Host 的回调
 * --------------------------------------------------------------------------
 * ========================================================================== */
static void host_on_connected(void* user, uint32_t connection_id)
{
    HostState* st = (HostState*)user;
    atomic_store(&st->conn_id, connection_id);
    atomic_store(&st->have_conn, 1);

    /* 此刻 TCP bootstrap + UDP/KCP 握手都已完成，可以安全 send */
    printf("[host] on_connected(connectionId=%u)\n", connection_id);
    printf("       └ Ork 已完成：TCP 握手 → 下发 BootstrapFrame → UDP 握手 → KCP ACK\n");
    printf("       └ 业务方可做：建会话、发欢迎包、记审计日志\n");
    fflush(stdout);
}

static void host_on_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    HostState* st = (HostState*)user;
    atomic_fetch_add(&st->disconnections, 1);
    atomic_store(&st->disconnect_reason, reason);
    printf("[host] on_disconnected(connectionId=%u, reason=%s)\n",
           connection_id, ork_demo_disconnect_reason_name((OrkDisconnectReason)reason));
    fflush(stdout);
}

/**
 * @brief 收到字节 —— 注意这里做的是**业务层命令解析**
 *
 * Ork 只保证"这些字节从 connection_id 上完整到达"，至于 "PING" 是什么意思，
 * 完全由这里决定。这就是"Ork 不理解业务"的具体形态。
 */
static void host_on_payload(void* user, uint32_t connection_id,
                            const uint8_t* data, uint32_t size, uint32_t channel)
{
    HostState* st = (HostState*)user;
    atomic_fetch_add(&st->payloads, 1);

    char text[LINE_MAX_LEN];
    const uint32_t n = size < (LINE_MAX_LEN - 1u) ? size : (LINE_MAX_LEN - 1u);
    memcpy(text, data, n);
    text[n] = '\0';

    printf("[host] on_payload(%u B, channel=%s): \"%s\"\n",
           size, ork_demo_channel_name((OrkChannel)channel), text);

    if (strcmp(text, CMD_PING) == 0)
    {
        const OrkSendResult r = ork_host_send(g_host, connection_id,
                                              (const uint8_t*)RSP_PONG,
                                              (uint32_t)strlen(RSP_PONG),
                                              OrkChannel_Reliable);
        printf("       └ 业务识别为 PING → 回 PONG（send=%s）\n",
               ork_demo_send_result_name(r));
    }
    else if (strncmp(text, CMD_ECHO, strlen(CMD_ECHO)) == 0)
    {
        /* 原样回送 "ECHO:" 后面的内容，同通道返回，便于 demo 观察 channel */
        const char* body = text + strlen(CMD_ECHO);
        const OrkSendResult r = ork_host_send(g_host, connection_id,
                                              (const uint8_t*)body,
                                              (uint32_t)strlen(body),
                                              (OrkChannel)channel);
        printf("       └ 业务识别为 ECHO → 回送 \"%s\"（send=%s）\n",
               body, ork_demo_send_result_name(r));
    }
    else if (strcmp(text, CMD_QUIT) == 0)
    {
        /* 业务决定关人：调用 ork_host_disconnect。回调里调用是安全的
         * （内部走异步 close，不会等待本线程）。 */
        const OrkError e = ork_host_disconnect(g_host, connection_id, OrkDisconnect_Kicked);
        atomic_store(&st->kicked, 1);
        printf("       └ 业务识别为 QUIT → 主动断开该连接（disconnect=%s）\n",
               e == OrkOk ? "Ok" : "BadState");
    }
    else
    {
        printf("       └ 未知命令，忽略（Ork 本身不会因为未知字节做任何事）\n");
    }
    fflush(stdout);
}

/* ========================================================================== */
/* --------------------------------------------------------------------------
 * Client 的回调
 * --------------------------------------------------------------------------
 * ========================================================================== */
static void client_on_connected(void* user, uint32_t connection_id)
{
    ClientState* st = (ClientState*)user;
    st->connected = 1;
    st->conn_id   = connection_id;
    printf("[client] on_connected(connectionId=%u) —— 现在可以 send 了\n", connection_id);
    fflush(stdout);
}

static void client_on_disconnected(void* user, uint32_t reason)
{
    ClientState* st = (ClientState*)user;
    st->disconnected = 1;
    st->reason       = reason;
    printf("[client] on_disconnected(reason=%s(%u))\n",
           ork_demo_disconnect_reason_name((OrkDisconnectReason)reason), reason);
    fflush(stdout);
}

static void client_on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    ClientState* st = (ClientState*)user;
    st->payloads++;
    st->last_channel = channel;

    const uint32_t n = size < (LINE_MAX_LEN - 1u) ? size : (LINE_MAX_LEN - 1u);
    memcpy(st->last_text, data, n);
    st->last_text[n] = '\0';

    printf("[client] on_payload(%u B, channel=%s): \"%s\"\n",
           size, ork_demo_channel_name((OrkChannel)channel), st->last_text);

    if (strcmp(st->last_text, RSP_PONG) == 0) st->got_pong = 1;
    else if (channel == (uint32_t)OrkChannel_Reliable)   st->got_echo_rel = 1;
    else                                                 st->got_echo_unrel = 1;
    fflush(stdout);
}

/* ========================================================================== */
/* 主流程小工具                                                                */
/* ========================================================================== */

/** @brief 驱动客户端若干拍，直到 pred 为真 / 超时 / 断开 */
typedef int (*WaitPred)(const ClientState* st, void* arg);

static int pump_until(OrkClientHandle client, ClientState* st,
                      WaitPred pred, void* arg, int timeout_ms)
{
    const uint64_t deadline = ork_demo_now_ms() + (uint64_t)timeout_ms;
    for (;;)
    {
        if (pred != NULL && pred(st, arg)) return 1;
        if (st->disconnected) return pred == NULL ? 1 : 0;

        (void)ork_client_pump_once(client);
        if (ork_demo_now_ms() >= deadline) return 0;
        ork_demo_sleep_ms(2);
    }
}

static int pred_connected(const ClientState* st, void* arg)  { (void)arg; return st->connected; }
static int pred_got_pong(const ClientState* st, void* arg)   { (void)arg; return st->got_pong; }

static void usage(const char* prog)
{
    printf("用法: %s [--timeout SEC] [--no-encryption] [--help]\n"
           "  --timeout SEC    每步等待上限（默认 10）\n"
           "  --no-encryption  关掉 AEAD（默认开）\n"
           "\n环境变量:\n"
           "  ORK_LOG_FILE   内部日志文件（默认 %s）\n"
           "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
           prog, ORK_DEVNULL);
}

/* ========================================================================== */
int main(int argc, char** argv)
{
    int timeout_s  = 10;
    int encryption = 1;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(argv[0]); return 0; }
        if (strcmp(a, "--no-encryption") == 0) { encryption = 0; continue; }
        if (strcmp(a, "--timeout") == 0 && i + 1 < argc)
        {
            const long v = strtol(argv[++i], NULL, 10);
            if (v < 1 || v > 600) { fprintf(stderr, "[demo] --timeout 允许 1..600\n"); return 2; }
            timeout_s = (int)v;
            continue;
        }
        fprintf(stderr, "[demo] 未知参数 %s\n", a);
        usage(argv[0]);
        return 2;
    }

    printf("%s\n", ORK_BANNER_RULE);
    printf(" Demo 07 · C ABI 回调与生命周期教程（真实 Host + Client，纯 C11）\n");
    printf(" ORK_ABI_VERSION=%u  encryption=%s\n", ork_abi_version(), encryption ? "on" : "off");
    printf(" 三步：on_connected → 应用层命令(PING/ECHO) → on_disconnected\n");
    printf("%s\n", ORK_BANNER_RULE);
    fflush(stdout);

    if (ork_abi_version() != ORK_ABI_VERSION)
    {
        fprintf(stderr, "[demo] C ABI 版本不匹配，头文件与归档不同源\n");
        return 1;
    }

    /* ---- 0. 端口 -------------------------------------------------------- */
    const uint16_t port = ork_demo_pick_free_port();
    if (port == 0) { fprintf(stderr, "[demo] 取空闲端口失败\n"); return 1; }
    printf("[demo] loopback 端口 = %u (TCP + UDP 共用)\n", (unsigned)port);

    /* ---- 1. Host -------------------------------------------------------- */
    HostState hst;
    memset(&hst, 0, sizeof(hst));
    atomic_store(&hst.conn_id, 0u);

    OrkHostConfig hcfg;
    ork_demo_fill_host_config(&hcfg, port, encryption);
    hcfg.log_path  = ork_demo_env_log_file();
    hcfg.log_level = ork_demo_env_log_level("demo");

    OrkHostCallbacks hcb = ORK_HOST_CALLBACKS_INIT;
    hcb.user            = &hst;
    hcb.on_connected    = host_on_connected;
    hcb.on_disconnected = host_on_disconnected;
    hcb.on_payload      = host_on_payload;

    if (ork_host_create(&hcfg, &hcb, &g_host) != OrkOk || g_host == NULL)
    {
        fprintf(stderr, "[demo] ork_host_create 失败: %s\n", ork_last_error());
        return 1;
    }
    hst.host = g_host;

    if (ork_host_start(g_host) != OrkOk)
    {
        fprintf(stderr, "[demo] ork_host_start 失败: %s\n", ork_last_error());
        ork_host_destroy(g_host);
        return 1;
    }

    /* ---- 2. Client ------------------------------------------------------ */
    ClientState cst;
    memset(&cst, 0, sizeof(cst));

    OrkClientConfig ccfg;
    ork_demo_fill_client_config(&ccfg, port, encryption, 5000);

    OrkClientCallbacks ccb = ORK_CLIENT_CALLBACKS_INIT;
    ccb.user            = &cst;
    ccb.on_connected    = client_on_connected;
    ccb.on_disconnected = client_on_disconnected;
    ccb.on_payload      = client_on_payload;

    OrkClientHandle client = NULL;
    int rc = 1;

    if (ork_client_create(&ccfg, &ccb, &client) != OrkOk || client == NULL)
    {
        fprintf(stderr, "[demo] ork_client_create 失败: %s\n", ork_client_last_error());
        ork_host_stop(g_host);
        ork_host_destroy(g_host);
        return 1;
    }

    /* ====================================================================== */
    /* Step 1：建立连接 → on_connected 两端各触发一次                         */
    /* ====================================================================== */
    printf("\n%s\n Step 1 · on_connected\n%s\n", ORK_BANNER_RULE, ORK_BANNER_RULE);
    if (ork_client_connect(client) != OrkOk)
    {
        fprintf(stderr, "[demo] ork_client_connect 失败: %s\n", ork_client_last_error());
        goto done;
    }
    if (!pump_until(client, &cst, pred_connected, NULL, timeout_s * 1000))
    {
        fprintf(stderr, "[demo] 连接超时\n");
        goto done;
    }
    if (!atomic_load(&hst.have_conn))
    {
        /* host 回调在内部线程，稍微再等一拍 */
        const uint64_t t = ork_demo_now_ms() + 1000u;
        while (!atomic_load(&hst.have_conn) && ork_demo_now_ms() < t) ork_demo_sleep_ms(5);
    }
    printf(" [观察] 客户端 connectionId=%u，host 侧 connectionId=%u（两端一致）\n",
           cst.conn_id, atomic_load(&hst.conn_id));

    /* ====================================================================== */
    /* Step 2：应用层命令 PING → PONG（Reliable）                             */
    /* ====================================================================== */
    printf("\n%s\n Step 2 · 应用层命令 PING → PONG（Ork 不理解 PING）\n%s\n",
           ORK_BANNER_RULE, ORK_BANNER_RULE);
    {
        const OrkSendResult r = ork_client_send(client, (const uint8_t*)CMD_PING,
                                                (uint32_t)strlen(CMD_PING),
                                                OrkChannel_Reliable);
        printf("[client] -> \"%s\" via Reliable  send=%s\n",
               CMD_PING, ork_demo_send_result_name(r));
        if (!pump_until(client, &cst, pred_got_pong, NULL, timeout_s * 1000))
        {
            fprintf(stderr, "[demo] 未在超时内收到 PONG\n");
            goto done;
        }
        printf(" [观察] 收到 PONG —— 命令语义完全由业务两侧约定，Ork 只搬运字节\n");
    }

    /* ====================================================================== */
    /* Step 3：Reliable 与 Unreliable 两条通道各走一次 ECHO                   */
    /* ====================================================================== */
    printf("\n%s\n Step 3 · 可靠(KCP) 与 不可靠(UDP) 通道\n%s\n",
           ORK_BANNER_RULE, ORK_BANNER_RULE);
    {
        const char* rel_msg = CMD_ECHO "reliable-path";
        const char* unrl_msg = CMD_ECHO "unreliable-path";

        const OrkSendResult r1 = ork_client_send(client, (const uint8_t*)rel_msg,
                                                 (uint32_t)strlen(rel_msg),
                                                 OrkChannel_Reliable);
        printf("[client] -> \"%s\" via Reliable  send=%s\n",
               rel_msg, ork_demo_send_result_name(r1));

        /* Unreliable 走裸 UDP，前几帧可能丢；重试几次是正常做法 */
        int attempt = 0;
        const OrkSendResult r2 = ork_client_send(client, (const uint8_t*)unrl_msg,
                                                 (uint32_t)strlen(unrl_msg),
                                                 OrkChannel_Unreliable);
        ++attempt;
        printf("[client] -> \"%s\" via Unreliable send=%s\n",
               unrl_msg, ork_demo_send_result_name(r2));

        /* 等两条通道的回包；Unreliable 没回就再补发（最多 5 次） */
        const uint64_t deadline = ork_demo_now_ms() + (uint64_t)timeout_s * 1000u;
        while (!(cst.got_echo_rel && cst.got_echo_unrel) && ork_demo_now_ms() < deadline)
        {
            (void)ork_client_pump_once(client);
            if (!cst.got_echo_unrel && attempt < 5
                && (ork_demo_now_ms() % 300u) < 20u)
            {
                (void)ork_client_send(client, (const uint8_t*)unrl_msg,
                                      (uint32_t)strlen(unrl_msg), OrkChannel_Unreliable);
                ++attempt;
            }
            ork_demo_sleep_ms(5);
        }
        printf(" [观察] Reliable 回包=%s（有序可靠），Unreliable 回包=%s（可能丢，需业务容错）\n",
               cst.got_echo_rel ? "收到" : "未收到",
               cst.got_echo_unrel ? "收到" : "未收到");
        if (!cst.got_echo_rel)
        {
            fprintf(stderr, "[demo] Reliable 通道未收到回包\n");
            goto done;
        }
        /* Unreliable 在 loopback 上通常也能到；收不到只记为告警，不影响 PASS */
        if (!cst.got_echo_unrel)
            printf(" [提示] Unreliable 未回包属正常（裸 UDP 不保证送达），本步不作硬断言\n");
    }

    /* ====================================================================== */
    /* Step 4：业务主动踢人 → 宿主连接回收；客户端侧的 on_disconnected 异步    */
    /* ====================================================================== */
    printf("\n%s\n Step 4 · 宿主 ork_host_disconnect（业务踢人）\n%s\n",
           ORK_BANNER_RULE, ORK_BANNER_RULE);
    {
        const OrkSendResult r = ork_client_send(client, (const uint8_t*)CMD_QUIT,
                                                (uint32_t)strlen(CMD_QUIT),
                                                OrkChannel_Reliable);
        printf("[client] -> \"%s\" via Reliable  send=%s\n",
               CMD_QUIT, ork_demo_send_result_name(r));

        /* 宿主侧是立即生效的：等连接数归零 + host OnDisconnected 落地 */
        const uint64_t deadline = ork_demo_now_ms() + (uint64_t)timeout_s * 1000u;
        while (ork_host_connection_count(g_host) != 0 && ork_demo_now_ms() < deadline)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(5);
        }
        if (ork_host_connection_count(g_host) != 0)
        {
            fprintf(stderr, "[demo] 宿主未能回收连接\n");
            goto done;
        }

        /* 客户端侧感知由协议超时驱动（TCP/KCP 关闭通知），ABI 不保证即时。
         * 这里只再多 pump 一小段时间，能收到就打出来，收不到也不判失败。 */
        const uint64_t grace = ork_demo_now_ms() + 1500u;
        while (!cst.disconnected && ork_demo_now_ms() < grace)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(5);
        }

        printf(" [观察] host 侧已断开：reason=%s，在线连接=%u\n",
               ork_demo_disconnect_reason_name(
                   (OrkDisconnectReason)atomic_load(&hst.disconnect_reason)),
               ork_host_connection_count(g_host));
        if (cst.disconnected)
            printf(" [观察] 客户端 on_disconnected 已在演示窗口内到达：reason=%s\n",
                   ork_demo_disconnect_reason_name((OrkDisconnectReason)cst.reason));
        else
            printf(" [提示] 客户端尚未感知断开 —— 属正常：C ABI 不保证被踢方立刻收到\n"
                   "        通知，客户端通常靠协议超时或自己的心跳/重连策略发现。\n");
    }

    /* ---- 结论 ----------------------------------------------------------- */
    {
        /* 宿主侧踢人一定成功；客户端感知不做硬断言（见 Step 4 说明） */
        const int kick_ok   = (atomic_load(&hst.disconnect_reason) == (uint32_t)OrkDisconnect_Kicked);
        const int host_disc = atomic_load(&hst.disconnections) >= 1;
        const int pass = cst.got_pong && cst.got_echo_rel && kick_ok && host_disc
                      && atomic_load(&hst.payloads) >= 3;

        printf("\n%s\n", ORK_BANNER_RULE);
        printf(" 回调观察汇总:\n");
        printf("   on_connected      client=%d  host=%d\n",
               cst.connected, atomic_load(&hst.have_conn) ? 1 : 0);
        printf("   on_payload        client=%u  host=%llu（PING/ECHO/QUIT 各一条）\n",
               cst.payloads, (unsigned long long)atomic_load(&hst.payloads));
        printf("   on_disconnected   host=%llu reason=%s  |  client=%d（异步感知）\n",
               (unsigned long long)atomic_load(&hst.disconnections),
               ork_demo_disconnect_reason_name(
                   (OrkDisconnectReason)atomic_load(&hst.disconnect_reason)),
               cst.disconnected);
        printf("   Reliable / Unreliable 通道：R=%s U=%s\n",
               cst.got_echo_rel ? "OK" : "FAIL",
               cst.got_echo_unrel ? "OK" : "n/a");
        printf("%s\n", pass ? "PASS: C ABI callbacks & lifecycle"
                            : "FAIL: 回调序列或断开结果不符");
        rc = pass ? 0 : 1;
    }

done:
    /* ---- 收尾：只在这里 stop / destroy ----------------------------------- */
    if (client != NULL)
    {
        (void)ork_client_disconnect(client);
        const uint64_t drain = ork_demo_now_ms() + 1500u;
        while (!cst.disconnected && ork_demo_now_ms() < drain)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(5);
        }
        ork_client_destroy(client);
    }
    ork_host_stop(g_host);
    ork_host_destroy(g_host);
    g_host = NULL;
    printf("[demo] 清理完成（stop → destroy 未 hang / 未崩）\n");
    return rc;
}
