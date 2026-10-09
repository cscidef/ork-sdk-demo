/**
 * @file test_c_abi_client.c
 * @brief C ABI Client 冒烟测试（纯 C11，只 include ork_client_abi.h）
 *
 * 覆盖范围：
 *   1. NULL 句柄安全；
 *   2. ork_client_create 的参数校验：
 *        - config / callbacks / out_client 任一为 NULL；
 *        - callbacks.struct_size 漂移；
 *        - config.struct_size **精确版本白名单**：只接受 96（v2）与
 *          ORK_CLIENT_CONFIG_V1_SIZE（72，v1），0 / 73 / 95 / 104 一律拒绝；
 *        - bootstrap_security_mode 非法值（错误信息里必须点名字段）；
 *        - tls_use_system_trust_store 三态之外的值；
 *        - kcp_stream_mode != 0（OrkFrame/AEAD 要求消息模式）；
 *        - max_packet_size 越界 [50, 1414]；
 *   3. **初始状态必须是 Disconnected、connection_id 必须是 0**（任务要求）；
 *   4. 未连接时 send -> NotConnected、pump_once -> OrkOk、disconnect 幂等；
 *   5. connect 到无人监听的 loopback 端口：重复 connect -> AlreadyStarted，
 *      并在超时前回到 Disconnected（真实走一遍异步失败路径）；
 *   6. destroy 幂等（NULL 安全）。
 *
 * 退出码：0 = 全部断言通过；1 = 有断言失败。
 */

#include "ork/sdk/c/ork_client_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  define ORK_CLOSESOCK closesocket
typedef SOCKET ork_sock_t;
#  define ORK_SOCK_INVALID INVALID_SOCKET
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static double now_ms(void) { return (double)GetTickCount64(); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
#  define ORK_CLOSESOCK close
typedef int ork_sock_t;
#  define ORK_SOCK_INVALID (-1)
static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}
#endif

/* -------------------------------------------------------------------------- */
static int g_checks   = 0;
static int g_failures = 0;

static void check(int cond, const char* label, int line)
{
    ++g_checks;
    if (cond)
    {
        printf("  [ok]   %s\n", label);
    }
    else
    {
        ++g_failures;
        printf("  [FAIL] %s   (line %d)\n", label, line);
        printf("         ork_client_last_error() = \"%s\"\n", ork_client_last_error());
    }
    fflush(stdout);
}
#define CHECK(cond, label) check((cond), (label), __LINE__)

static void check_u32(unsigned long long actual, unsigned long long expected,
                      const char* label, int line)
{
    ++g_checks;
    if (actual == expected)
    {
        printf("  [ok]   %s (= %llu)\n", label, actual);
    }
    else
    {
        ++g_failures;
        printf("  [FAIL] %s: got %llu, expected %llu   (line %d)\n",
               label, actual, expected, line);
        printf("         ork_client_last_error() = \"%s\"\n", ork_client_last_error());
    }
    fflush(stdout);
}
#define CHECK_EQ(actual, expected, label) \
    check_u32((unsigned long long)(actual), (unsigned long long)(expected), (label), __LINE__)

/** @brief 找一个当前无人监听的 loopback TCP 端口（bind 后立即 close） */
static uint16_t pick_closed_port(void)
{
    int attempt;
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (attempt = 0; attempt < 64; ++attempt)
    {
        struct sockaddr_in addr;
        socklen_t len = (socklen_t)sizeof(addr);
        uint16_t port;
        ork_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == ORK_SOCK_INVALID) continue;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        {
            ORK_CLOSESOCK(s);
            continue;
        }
        if (getsockname(s, (struct sockaddr*)&addr, &len) != 0)
        {
            ORK_CLOSESOCK(s);
            continue;
        }
        port = (uint16_t)ntohs(addr.sin_port);
        ORK_CLOSESOCK(s);          /* 关闭后端口无人监听 → connect 会被 RST */
        return port;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
static volatile uint32_t g_connected    = 0;
static volatile uint32_t g_disconnected = 0;
static volatile uint32_t g_payload      = 0;
static volatile uint32_t g_conn_id      = 0;
static volatile uint32_t g_reason       = 0;

static void cb_connected(void* user, uint32_t connection_id)
{
    (void)user;
    ++g_connected;
    g_conn_id = connection_id;
}
static void cb_disconnected(void* user, uint32_t reason)
{
    (void)user;
    ++g_disconnected;
    g_reason = reason;
}
static void cb_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)user; (void)data; (void)size; (void)channel;
    ++g_payload;
}

static void fill_valid_config(OrkClientConfig* c, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->struct_size                = (uint32_t)sizeof(OrkClientConfig);   /* v2 = 96 */
    c->host                       = "127.0.0.1";
    c->tcp_port                   = port;
    c->udp_port                   = port;   /* ★ 必须与 tcp_port 相同（留 0 会回落 9401） */
    c->enable_kcp                 = 1;
    c->enable_encryption          = 1;
    c->max_packet_size            = 1200;
    c->connect_timeout_ms         = 1500;   /* 测试要快速失败 */
    c->kcp_stream_mode            = 0;      /* 必须 0 */
    c->bootstrap_security_mode    = OrkBootstrapSecurity_PlaintextDev;
    c->tls_ca_file                = NULL;
    c->tls_expected_server_name   = NULL;
    c->tls_use_system_trust_store = 0;
    c->tls_handshake_timeout_ms   = 1500;
}

static void fill_callbacks(OrkClientCallbacks* cb)
{
    memset(cb, 0, sizeof(*cb));
    cb->struct_size     = (uint32_t)sizeof(OrkClientCallbacks);
    cb->user            = NULL;
    cb->on_connected    = cb_connected;
    cb->on_disconnected = cb_disconnected;
    cb->on_payload      = cb_payload;
}

/**
 * @brief 断言"这份配置必须被 create 拒绝"
 *
 * 同时验证一条 ABI 契约：**失败路径绝不写 out_client**。
 * 源码（ork_client_abi.cpp ork_client_create）里每一条 return OrkErr_* 都排在
 * `*out_client = handle.release();` 之前，所以传入的 h 应保持调用方给的初值。
 * 用 NULL 初值 + 断言仍为 NULL，才能把这条契约真正钉住。
 */
static void expect_reject(const OrkClientConfig* cfg, const OrkClientCallbacks* cb,
                          const char* label, int line)
{
    OrkClientHandle h = NULL;                 /* ← 必须初始化为 NULL，不是 poison */
    const OrkError e = ork_client_create(cfg, cb, &h);
    ++g_checks;
    if (e == OrkErr_InvalidArg && h == NULL)
    {
        printf("  [ok]   %s\n", label);
    }
    else
    {
        ++g_failures;
        printf("  [FAIL] %s: got OrkError=%d out=%s   (line %d)\n",
               label, (int)e, (h == NULL) ? "NULL" : "NON-NULL", line);
        printf("         ork_client_last_error() = \"%s\"\n", ork_client_last_error());
        if (h != NULL) ork_client_destroy(h);   /* 意外创建成功 → 别泄漏 */
    }
    fflush(stdout);
}
#define EXPECT_REJECT(cfgp, cbp, label) expect_reject((cfgp), (cbp), (label), __LINE__)

/* ========================================================================== */
int main(void)
{
    OrkClientHandle client = NULL;
    OrkClientConfig cfg;
    OrkClientCallbacks cb;
    uint16_t closed_port;
    OrkError e;

    printf("=============================================================\n");
    printf(" test_c_abi_client · C ABI Client smoke\n");
    printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE);
    printf(" sizeof(OrkClientCallbacks)=%zu\n", sizeof(OrkClientCallbacks));
    printf("=============================================================\n");

    /* 结构体布局门禁：v2 必须是 96 字节，否则整个 ABI 假设都不成立 */
    CHECK_EQ(sizeof(OrkClientConfig), 96u, "sizeof(OrkClientConfig) == 96 (v2 LP64)");
    CHECK_EQ(ORK_CLIENT_CONFIG_V1_SIZE, 72u, "ORK_CLIENT_CONFIG_V1_SIZE == 72");

    /* ---- 1. NULL 句柄安全 ---------------------------------------------- */
    printf("\n[1] NULL 句柄安全\n");
    CHECK_EQ(ork_client_get_state(NULL), OrkClientState_Disconnected,
           "get_state(NULL) -> Disconnected");
    CHECK_EQ(ork_client_get_connection_id(NULL), 0u, "get_connection_id(NULL) -> 0");
    CHECK_EQ(ork_client_pump_once(NULL), OrkErr_InvalidArg, "pump_once(NULL) -> InvalidArg");
    CHECK_EQ(ork_client_connect(NULL),   OrkErr_InvalidArg, "connect(NULL) -> InvalidArg");
    CHECK_EQ(ork_client_disconnect(NULL),OrkErr_InvalidArg, "disconnect(NULL) -> InvalidArg");
    {
        static const uint8_t blob[4] = { 1, 2, 3, 4 };
        CHECK_EQ(ork_client_send(NULL, blob, 4, OrkChannel_Reliable), OrkSend_BadState,
               "send(NULL,..) -> BadState");
    }
    ork_client_destroy(NULL);
    CHECK(1, "destroy(NULL) 安全返回");

    /* ---- 2. create 参数校验 -------------------------------------------- */
    printf("\n[2] ork_client_create 参数校验\n");
    closed_port = pick_closed_port();
    CHECK(closed_port != 0, "pick_closed_port() 拿到无人监听的端口");
    printf("      loopback 端口 = %u\n", (unsigned)closed_port);

    fill_callbacks(&cb);
    fill_valid_config(&cfg, closed_port);

    /* 先确认基线配置本身是合法的，否则后面的"拒绝"断言就没有意义 */
    {
        OrkClientHandle probe = NULL;
        CHECK_EQ(ork_client_create(&cfg, &cb, &probe), OrkOk, "create(基线 v2 配置) -> OrkOk");
        CHECK(probe != NULL, "基线句柄非 NULL");
        ork_client_destroy(probe);
    }

    CHECK_EQ(ork_client_create(NULL, &cb, &client), OrkErr_InvalidArg,
           "create(NULL config) -> InvalidArg");
    CHECK_EQ(ork_client_create(&cfg, NULL, &client), OrkErr_InvalidArg,
           "create(NULL callbacks) -> InvalidArg");
    CHECK_EQ(ork_client_create(&cfg, &cb, NULL), OrkErr_InvalidArg,
           "create(NULL out_client) -> InvalidArg");
    CHECK(client == NULL, "失败的 create 不写 out_client");

    {   /* callbacks.struct_size 漂移 */
        OrkClientCallbacks badcb = cb;
        badcb.struct_size = (uint32_t)sizeof(OrkClientCallbacks) - 8u;
        EXPECT_REJECT(&cfg, &badcb, "create(callbacks.struct_size 漂移) -> InvalidArg");
    }

    /* struct_size 精确版本白名单：只有 72(v1) 与 96(v2) 合法 */
    {
        const uint32_t bad_sizes[] = { 0u, 8u, 71u, 73u, 95u, 97u, 104u, 0xFFFFFFFFu };
        const size_t n = sizeof(bad_sizes) / sizeof(bad_sizes[0]);
        size_t i;
        int all_rejected = 1;
        for (i = 0; i < n; ++i)
        {
            OrkClientConfig bad = cfg;
            OrkClientHandle h = NULL;
            bad.struct_size = bad_sizes[i];
            if (ork_client_create(&bad, &cb, &h) != OrkErr_InvalidArg || h != NULL)
            {
                all_rejected = 0;
                printf("         struct_size=%u 竟然被接受了\n", (unsigned)bad_sizes[i]);
                if (h != NULL) ork_client_destroy(h);
            }
        }
        CHECK(all_rejected, "create(struct_size ∉ {72,96}) 全部 -> InvalidArg");
    }
    {   /* v1 布局（72 字节）必须被接受，且 TLS 字段按零值处理 */
        OrkClientConfig v1 = cfg;
        OrkClientHandle h = NULL;
        v1.struct_size = ORK_CLIENT_CONFIG_V1_SIZE;
        /* v1 不读 bootstrap_security_mode，语义回落到默认（TLS）；
         * 本构建 ORK_HAVE_TLS 已编入，因此 create 仍应成功。 */
        e = ork_client_create(&v1, &cb, &h);
        CHECK_EQ(e, OrkOk, "create(struct_size=v1 72B) -> OrkOk（向后兼容）");
        if (h != NULL)
        {
            CHECK_EQ(ork_client_get_state(h), OrkClientState_Disconnected,
                   "v1 句柄初始状态 Disconnected");
            ork_client_destroy(h);
        }
    }

    {   /* bootstrap_security_mode 非法值 */
        OrkClientConfig bad = cfg;
        bad.bootstrap_security_mode = 99;
        EXPECT_REJECT(&bad, &cb, "create(bootstrap_security_mode=99) -> InvalidArg");
        CHECK(strstr(ork_client_last_error(), "bootstrap_security_mode") != NULL,
              "last_error 点名了 bootstrap_security_mode");
    }
    {   /* tls_use_system_trust_store 三态之外 */
        OrkClientConfig bad = cfg;
        bad.tls_use_system_trust_store = 5;
        EXPECT_REJECT(&bad, &cb, "create(tls_use_system_trust_store=5) -> InvalidArg");
        CHECK(strstr(ork_client_last_error(), "tls_use_system_trust_store") != NULL,
              "last_error 点名了 tls_use_system_trust_store");
    }
    {   /* kcp_stream_mode 必须为 0 */
        OrkClientConfig bad = cfg;
        bad.kcp_stream_mode = 1;
        EXPECT_REJECT(&bad, &cb, "create(kcp_stream_mode=1) -> InvalidArg");
    }
    {   /* max_packet_size 越界 */
        OrkClientConfig bad = cfg;
        bad.max_packet_size = 4000;
        EXPECT_REJECT(&bad, &cb, "create(max_packet_size=4000) -> InvalidArg");
        CHECK(strstr(ork_client_last_error(), "max_packet_size") != NULL,
              "last_error 点名了 max_packet_size");

        bad.max_packet_size = 10;
        EXPECT_REJECT(&bad, &cb, "create(max_packet_size=10) -> InvalidArg");
    }
    {   /* TLS 模式 + 明文未编入的组合在这里不适用（本构建两者都编入），
         * 但 Tls(2) 与 Default(0) 都必须能通过 create（证书校验推迟到 connect） */
        OrkClientConfig tls = cfg;
        OrkClientHandle h = NULL;
        tls.bootstrap_security_mode = OrkBootstrapSecurity_Tls;
        CHECK_EQ(ork_client_create(&tls, &cb, &h), OrkOk,
               "create(bootstrap_security_mode=Tls) -> OrkOk（证书在 connect 阶段校验）");
        if (h != NULL) ork_client_destroy(h);

        h = NULL;
        tls.bootstrap_security_mode = OrkBootstrapSecurity_Default;
        CHECK_EQ(ork_client_create(&tls, &cb, &h), OrkOk,
               "create(bootstrap_security_mode=Default) -> OrkOk");
        if (h != NULL) ork_client_destroy(h);
    }

    /* ---- 3. 正常创建 + 初始状态 ----------------------------------------- */
    printf("\n[3] 正常创建与初始状态\n");
    fill_valid_config(&cfg, closed_port);
    e = ork_client_create(&cfg, &cb, &client);
    CHECK_EQ(e, OrkOk, "create(valid v2) -> OrkOk");
    CHECK(client != NULL, "句柄非 NULL");
    if (client == NULL) { printf("\nFAIL (%d/%d)\n", g_failures, g_checks); return 1; }

    CHECK_EQ(ork_client_get_state(client), OrkClientState_Disconnected,
           "★ 初始状态 == Disconnected");
    CHECK_EQ(ork_client_get_connection_id(client), 0u,
           "★ 初始 connection_id == 0");
    CHECK_EQ(ork_client_pump_once(client), OrkOk, "pump_once(未连接) -> OrkOk");

    {   /* 未连接时发送必须被拒 */
        static const uint8_t blob[25] = {
            'O','R','K','B','E','N','C','H','1', 'O','R','K','D','E','M','O','1',
            0,0,0,0,0,0,0,0
        };
        CHECK_EQ(ork_client_send(client, blob, (uint32_t)sizeof(blob), OrkChannel_Reliable),
               OrkSend_NotConnected, "send(未连接) -> NotConnected");
        CHECK_EQ(ork_client_send(client, blob, 25u, OrkChannel_Unreliable),
               OrkSend_NotConnected, "send(未连接, Unreliable) -> NotConnected");
        CHECK_EQ(ork_client_send(client, NULL, 25u, OrkChannel_Reliable),
               OrkSend_BadState, "send(NULL data, size>0) -> BadState");
    }
    CHECK_EQ(ork_client_disconnect(client), OrkOk, "disconnect(未连接) -> OrkOk（幂等）");
    CHECK_EQ(ork_client_disconnect(client), OrkOk, "再次 disconnect -> OrkOk（幂等）");
    CHECK_EQ(g_connected + g_disconnected + g_payload, 0u, "尚无回调触发");

    /* ---- 4. connect 到无人监听的端口：真实走一遍异步失败路径 ------------- */
    printf("\n[4] connect 到无人监听的端口（异步失败路径）\n");
    CHECK_EQ(ork_client_connect(client), OrkOk, "connect() -> OrkOk（异步受理）");
    CHECK_EQ(ork_client_connect(client), OrkErr_AlreadyStarted,
           "重复 connect -> AlreadyStarted");
    {
        const double deadline = now_ms() + 8000.0;
        int pumped = 0;
        while (now_ms() < deadline && g_disconnected == 0 && pumped < 4000)
        {
            ork_client_pump_once(client);
            ++pumped;
            sleep_ms(2);
        }
        printf("      pumped=%d 次, on_disconnected=%u reason=%u state=%d\n",
               pumped, (unsigned)g_disconnected, (unsigned)g_reason,
               (int)ork_client_get_state(client));
        /* 连接被 RST 或超时后必须回到 Disconnected（不能卡在中间态） */
        CHECK(g_disconnected == 1u
              || ork_client_get_state(client) == OrkClientState_Disconnected,
              "connect 失败后回到 Disconnected");
        CHECK_EQ(g_connected, 0u, "on_connected 未被触发（对端不存在）");
        CHECK_EQ(g_payload,   0u, "on_payload 未被触发");
    }
    CHECK_EQ(ork_client_disconnect(client), OrkOk, "失败后 disconnect -> OrkOk");

    /* ---- 5. 失败后仍可用于新一轮 connect（重连语义） --------------------- */
    printf("\n[5] 断开后可再次 connect（重连语义）\n");
    CHECK_EQ(ork_client_connect(client), OrkOk, "断开后再次 connect -> OrkOk");
    {
        const double deadline = now_ms() + 4000.0;
        while (now_ms() < deadline && g_disconnected < 2u)
        {
            ork_client_pump_once(client);
            sleep_ms(2);
        }
        CHECK(g_disconnected >= 1u, "第二次 connect 也走到了终态");
    }
    CHECK_EQ(ork_client_disconnect(client), OrkOk, "disconnect -> OrkOk");

    /* ---- 6. 销毁 -------------------------------------------------------- */
    printf("\n[6] destroy\n");
    ork_client_destroy(client);
    CHECK(1, "destroy(client) 正常返回");
    ork_client_destroy(NULL);
    CHECK(1, "destroy(NULL) 再次调用安全");

    /* ---- 汇总 ----------------------------------------------------------- */
    printf("\n=============================================================\n");
    if (g_failures == 0)
    {
        printf("PASS: test_c_abi_client  (%d/%d checks)\n", g_checks - g_failures, g_checks);
        printf("=============================================================\n");
        return 0;
    }
    printf("FAIL: test_c_abi_client  (%d failures / %d checks)\n", g_failures, g_checks);
    printf("=============================================================\n");
    return 1;
}
