/**
 * @file test_c_abi_host.c
 * @brief C ABI Host 冒烟测试（纯 C11，只 include ork_abi.h）
 *
 * 覆盖范围（全部离线，不需要任何对端）：
 *   1. ork_abi_version() 与编译期 ORK_ABI_VERSION 一致；
 *   2. NULL 句柄安全：所有函数传 NULL 都不崩、返回约定的错误码；
 *   3. ork_host_create 的参数校验：空指针 / struct_size 漂移 / drive_mode=1；
 *   4. start 之前的运行期 API 一律拒绝（NotStarted / BadState）；
 *   5. start 成功 + 重复 start 返回 AlreadyStarted；
 *   6. pump_once / connection_count / get_stats / get_connection_info /
 *      disconnect / broadcast 在"零连接"状态下的确定行为；
 *   7. stop 成功 + 重复 stop 返回 NotStarted；
 *   8. destroy 幂等（NULL 安全）。
 *
 * 监听端口在运行时向 OS 申请（TCP + UDP 同时空闲），因此可以 ctest -j 并行。
 *
 * 退出码：0 = 全部断言通过；1 = 有断言失败。
 */

#include "ork/sdk/c/ork_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define ORK_CLOSESOCK closesocket
#  define ORK_DEVNULL "NUL"
typedef SOCKET ork_sock_t;
#  define ORK_SOCK_INVALID INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define ORK_CLOSESOCK close
#  define ORK_DEVNULL "/dev/null"
typedef int ork_sock_t;
#  define ORK_SOCK_INVALID (-1)
#endif

/* -------------------------------------------------------------------------- */
/* 极简断言框架（不依赖任何第三方）                                            */
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
        printf("         ork_last_error() = \"%s\"\n", ork_last_error());
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
        printf("         ork_last_error() = \"%s\"\n", ork_last_error());
    }
    fflush(stdout);
}

#define CHECK_EQ(actual, expected, label) \
    check_u32((unsigned long long)(actual), (unsigned long long)(expected), (label), __LINE__)

/* -------------------------------------------------------------------------- */
/* 临时端口：TCP bootstrap 与 UDP/KCP 共用同一端口，必须两边都探                */
/* -------------------------------------------------------------------------- */
static uint16_t pick_free_port(void)
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
        ork_sock_t udp, tcp;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp == ORK_SOCK_INVALID) continue;
        if (bind(udp, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        {
            ORK_CLOSESOCK(udp);
            continue;
        }
        if (getsockname(udp, (struct sockaddr*)&addr, &len) != 0)
        {
            ORK_CLOSESOCK(udp);
            continue;
        }
        port = (uint16_t)ntohs(addr.sin_port);

        /* 同一端口再探 TCP */
        tcp = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp == ORK_SOCK_INVALID)
        {
            ORK_CLOSESOCK(udp);
            continue;
        }
        addr.sin_port = htons(port);
        if (bind(tcp, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        {
            ORK_CLOSESOCK(udp);
            ORK_CLOSESOCK(tcp);
            continue;
        }

        ORK_CLOSESOCK(udp);
        ORK_CLOSESOCK(tcp);
        return port;   /* 两个 socket 关闭后端口被释放（存在极小竞态窗口） */
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 回调：本测试不建连接，理论上不会被触发；一旦被触发就记录下来当失败信号       */
/* -------------------------------------------------------------------------- */
static volatile uint32_t g_cb_connected    = 0;
static volatile uint32_t g_cb_disconnected = 0;
static volatile uint32_t g_cb_payload      = 0;
static volatile uint32_t g_cb_stats        = 0;

static void cb_connected(void* user, uint32_t connection_id)
{
    (void)user; (void)connection_id;
    ++g_cb_connected;
}
static void cb_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    (void)user; (void)connection_id; (void)reason;
    ++g_cb_disconnected;
}
static void cb_payload(void* user, uint32_t connection_id,
                       const uint8_t* data, uint32_t size, uint32_t channel)
{
    (void)user; (void)connection_id; (void)data; (void)size; (void)channel;
    ++g_cb_payload;
}
static void cb_stats(void* user, const OrkStats* stats)
{
    (void)user; (void)stats;
    ++g_cb_stats;
}

static void fill_valid_config(OrkHostConfig* c, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->struct_size         = (uint32_t)sizeof(OrkHostConfig);
    c->tcp_port            = port;
    c->udp_port            = port;      /* 与 tcp_port 相同 */
    c->enable_kcp          = 1;
    c->enable_encryption   = 1;
    c->bootstrap_ttl_sec   = 300;
    c->pump_rate_hz        = 30;
    c->kcp_update_rate_hz  = 100;       /* 必须 >= pump_rate_hz */
    c->idle_timeout_sec    = 300;
    c->max_packet_size     = 1200;
    c->drive_mode          = 0;         /* 1 = External，当前实现显式拒绝 */
    c->log_path            = ORK_DEVNULL;
    c->log_level           = (uint32_t)OrkLog_Off;   /* 测试保持静音 */
}

/* ========================================================================== */
int main(void)
{
    OrkHost host = NULL;
    OrkHostConfig cfg;
    OrkHostCallbacks cb;
    uint16_t port;
    OrkError e;

    printf("=============================================================\n");
    printf(" test_c_abi_host · C ABI Host smoke\n");
    printf(" sizeof(OrkHostConfig)=%zu sizeof(OrkHostCallbacks)=%zu\n",
           sizeof(OrkHostConfig), sizeof(OrkHostCallbacks));
    printf(" sizeof(OrkConnectionInfo)=%zu sizeof(OrkStats)=%zu\n",
           sizeof(OrkConnectionInfo), sizeof(OrkStats));
    printf("=============================================================\n");

    /* ---- 1. ABI 版本 --------------------------------------------------- */
    printf("\n[1] ABI 版本\n");
    CHECK_EQ(ork_abi_version(), ORK_ABI_VERSION, "ork_abi_version() == ORK_ABI_VERSION");

    /* ---- 2. NULL 句柄安全 ---------------------------------------------- */
    printf("\n[2] NULL 句柄安全（不崩溃 + 约定错误码）\n");
    CHECK_EQ(ork_host_start(NULL),          OrkErr_InvalidArg, "start(NULL) -> InvalidArg");
    CHECK_EQ(ork_host_stop(NULL),           OrkErr_InvalidArg, "stop(NULL) -> InvalidArg");
    CHECK_EQ(ork_host_pump_once(NULL),      OrkErr_InvalidArg, "pump_once(NULL) -> InvalidArg");
    CHECK_EQ(ork_host_disconnect(NULL, 1, OrkDisconnect_Kicked),
           OrkErr_InvalidArg, "disconnect(NULL,..) -> InvalidArg");
    CHECK_EQ(ork_host_connection_count(NULL), 0u, "connection_count(NULL) -> 0");
    {
        static const uint8_t blob[4] = { 1, 2, 3, 4 };
        CHECK_EQ(ork_host_send(NULL, 1, blob, 4, OrkChannel_Reliable),
               OrkSend_BadState, "send(NULL,..) -> BadState");
        CHECK_EQ(ork_host_broadcast(NULL, blob, 4, OrkChannel_Reliable),
               OrkSend_BadState, "broadcast(NULL,..) -> BadState");
    }
    {
        OrkStats st;
        OrkConnectionInfo info;
        memset(&st, 0, sizeof(st));   st.struct_size = (uint32_t)sizeof(OrkStats);
        memset(&info, 0, sizeof(info)); info.struct_size = (uint32_t)sizeof(OrkConnectionInfo);
        CHECK_EQ(ork_host_get_stats(NULL, &st), OrkErr_InvalidArg,
               "get_stats(NULL,..) -> InvalidArg");
        CHECK_EQ(ork_host_get_connection_info(NULL, 1, &info), OrkErr_InvalidArg,
               "get_connection_info(NULL,..) -> InvalidArg");
    }
    ork_host_destroy(NULL);   /* 必须无操作返回 */
    CHECK(1, "destroy(NULL) 安全返回");

    /* ---- 3. create 参数校验 -------------------------------------------- */
    printf("\n[3] ork_host_create 参数校验\n");
    port = pick_free_port();
    CHECK(port != 0, "pick_free_port() 拿到临时端口");
    if (port == 0) { printf("\nFAIL (%d/%d)\n", g_failures, g_checks); return 1; }
    printf("      loopback 端口 = %u\n", (unsigned)port);

    memset(&cb, 0, sizeof(cb));
    cb.struct_size     = (uint32_t)sizeof(OrkHostCallbacks);
    cb.user            = NULL;
    cb.on_connected    = cb_connected;
    cb.on_disconnected = cb_disconnected;
    cb.on_payload      = cb_payload;
    cb.on_stats        = cb_stats;

    fill_valid_config(&cfg, port);

    CHECK_EQ(ork_host_create(NULL, &cb, &host), OrkErr_InvalidArg, "create(NULL config) -> InvalidArg");
    CHECK_EQ(ork_host_create(&cfg, NULL, &host), OrkErr_InvalidArg, "create(NULL callbacks) -> InvalidArg");
    CHECK_EQ(ork_host_create(&cfg, &cb, NULL),  OrkErr_InvalidArg, "create(NULL out) -> InvalidArg");
    CHECK(host == NULL, "失败的 create 不写 out_host");

    {   /* struct_size 漂移（模拟 ABI 版本不匹配） */
        OrkHostConfig bad = cfg;
        bad.struct_size = (uint32_t)sizeof(OrkHostConfig) - 4u;
        CHECK_EQ(ork_host_create(&bad, &cb, &host), OrkErr_InvalidArg,
               "create(struct_size 偏小) -> InvalidArg");
    }
    {
        OrkHostCallbacks badcb = cb;
        badcb.struct_size = (uint32_t)sizeof(OrkHostCallbacks) + 8u;
        CHECK_EQ(ork_host_create(&cfg, &badcb, &host), OrkErr_InvalidArg,
               "create(callbacks.struct_size 偏大) -> InvalidArg");
    }
    {   /* drive_mode=1（External）当前显式拒绝 */
        OrkHostConfig bad = cfg;
        bad.drive_mode = 1;
        CHECK_EQ(ork_host_create(&bad, &cb, &host), OrkErr_InvalidArg,
               "create(drive_mode=1) -> InvalidArg");
        CHECK(strstr(ork_last_error(), "External drive mode") != NULL
              || strstr(ork_last_error(), "drive") != NULL,
              "last_error 说明了 drive_mode 被拒原因");
    }
    {   /* kcp_update_rate_hz < pump_rate_hz：SessionFramework::Create 抛 → InvalidArg */
        OrkHostConfig bad = cfg;
        bad.pump_rate_hz       = 200;
        bad.kcp_update_rate_hz = 50;
        CHECK_EQ(ork_host_create(&bad, &cb, &host), OrkErr_InvalidArg,
               "create(kcp_update_rate < pump_rate) -> InvalidArg");
    }
    {   /* max_packet_size 越界 [50, 1414] */
        OrkHostConfig bad = cfg;
        bad.max_packet_size = 4000;
        CHECK_EQ(ork_host_create(&bad, &cb, &host), OrkErr_InvalidArg,
               "create(max_packet_size=4000) -> InvalidArg");
    }

    /* ---- 4. 正常创建 ---------------------------------------------------- */
    printf("\n[4] ork_host_create 正常路径\n");
    fill_valid_config(&cfg, port);
    e = ork_host_create(&cfg, &cb, &host);
    CHECK_EQ(e, OrkOk, "create(valid) -> OrkOk");
    CHECK(host != NULL, "out_host 非 NULL");
    if (host == NULL) { printf("\nFAIL (%d/%d)\n", g_failures, g_checks); return 1; }

    /* ---- 5. start 之前的运行期 API 一律拒绝 ------------------------------ */
    printf("\n[5] 未 start 时的运行期 API\n");
    CHECK_EQ(ork_host_pump_once(host), OrkErr_NotStarted, "pump_once(未 start) -> NotStarted");
    CHECK_EQ(ork_host_disconnect(host, 1, OrkDisconnect_Kicked), OrkErr_NotStarted,
           "disconnect(未 start) -> NotStarted");
    {
        static const uint8_t blob[4] = { 1, 2, 3, 4 };
        CHECK_EQ(ork_host_send(host, 1, blob, 4, OrkChannel_Reliable), OrkSend_BadState,
               "send(未 start) -> BadState");
        CHECK_EQ(ork_host_broadcast(host, blob, 4, OrkChannel_Reliable), OrkSend_BadState,
               "broadcast(未 start) -> BadState");
        CHECK_EQ(ork_host_send(host, 1, NULL, 4, OrkChannel_Reliable), OrkSend_BadState,
               "send(NULL data, size>0) -> BadState");
    }
    CHECK_EQ(ork_host_connection_count(host), 0u, "connection_count(未 start) == 0");
    {   /* get_stats 不要求 started：返回内部零值缓存 */
        OrkStats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(OrkStats);
        CHECK_EQ(ork_host_get_stats(host, &st), OrkOk, "get_stats(未 start) -> OrkOk");
        CHECK_EQ(st.struct_size, sizeof(OrkStats), "get_stats 回填 struct_size");
        CHECK_EQ(st.active_connections, 0u, "active_connections == 0");
    }
    {   /* struct_size 不符必须被拒 */
        OrkStats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = 1u;
        CHECK_EQ(ork_host_get_stats(host, &st), OrkErr_InvalidArg,
               "get_stats(struct_size 非法) -> InvalidArg");
    }

    /* ---- 6. start / 重复 start ------------------------------------------ */
    printf("\n[6] start / 重复 start\n");
    CHECK_EQ(ork_host_start(host), OrkOk, "start -> OrkOk");
    CHECK_EQ(ork_host_start(host), OrkErr_AlreadyStarted, "重复 start -> AlreadyStarted");
    CHECK_EQ(ork_host_pump_once(host), OrkOk, "pump_once(已 start) -> OrkOk");

    /* ---- 7. 零连接状态下的查询 / 发送 ------------------------------------ */
    printf("\n[7] 零连接状态下的查询与发送\n");
    CHECK_EQ(ork_host_connection_count(host), 0u, "connection_count == 0（无客户端）");
    {
        OrkConnectionInfo info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(OrkConnectionInfo);
        CHECK_EQ(ork_host_get_connection_info(host, 0xDEADBEEFu, &info), OrkErr_BadState,
               "get_connection_info(不存在的 id) -> BadState");
        info.struct_size = 1u;
        CHECK_EQ(ork_host_get_connection_info(host, 1, &info), OrkErr_InvalidArg,
               "get_connection_info(struct_size 非法) -> InvalidArg");
    }
    CHECK_EQ(ork_host_disconnect(host, 0xDEADBEEFu, OrkDisconnect_Kicked), OrkErr_BadState,
           "disconnect(不存在的 id) -> BadState");
    {
        static const uint8_t blob[8] = { 'O','R','K','T','E','S','T','1' };
        /* 无连接时广播是 vacuous 成功（见 ork_abi.h 注释） */
        CHECK_EQ(ork_host_broadcast(host, blob, 8, OrkChannel_Reliable), OrkSend_Ok,
               "broadcast(零连接) -> Ok（vacuous）");
        /* ★ ork_host_send 到不存在的连接返回 **BadState**，不是 NotConnected：
         *   Sender::Send 把实际发送委托给 NetworkTransportAdapter::TrySend(bool)，
         *   false 无法细分 NotConnected / QueueFull / CipherFailed / NoRoute，
         *   于是保守映射为 SendResult::BadState（见 sender.cpp 头部注释）。
         *   所以 C ABI 的 OrkSend_NotConnected 实际上只会从 ork_client_send
         *   （客户端未 Connected）产生，host 侧永远走不到。 */
        {
            const OrkSendResult sr = ork_host_send(host, 1, blob, 8, OrkChannel_Unreliable);
            CHECK_EQ(sr, OrkSend_BadState,
                   "send(不存在的连接) -> BadState（TrySend=false 的保守映射）");
            CHECK(sr != OrkSend_Ok, "send(不存在的连接) 绝不可返回 Ok");
        }
    }
    {
        OrkStats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(OrkStats);
        CHECK_EQ(ork_host_get_stats(host, &st), OrkOk, "get_stats(已 start) -> OrkOk");
        CHECK_EQ(st.active_connections, 0u, "stats.active_connections == 0");
        CHECK(st.crypto_fail_aead == 0 && st.crypto_fail_replay == 0
              && st.crypto_fail_expired == 0 && st.crypto_fail_malformed == 0,
              "stats.crypto_fail_* 全 0");
        /* disconnects_by_reason 必须是 8 槽且全 0 */
        {
            int i, all_zero = 1;
            for (i = 0; i < 8; ++i) if (st.disconnects_by_reason[i] != 0u) all_zero = 0;
            CHECK(all_zero, "stats.disconnects_by_reason[0..7] 全 0");
        }
    }

    /* ---- 8. stop / 重复 stop -------------------------------------------- */
    printf("\n[8] stop / 重复 stop / destroy\n");
    CHECK_EQ(ork_host_stop(host), OrkOk, "stop -> OrkOk");
    CHECK_EQ(ork_host_stop(host), OrkErr_NotStarted, "重复 stop -> NotStarted");
    CHECK_EQ(ork_host_pump_once(host), OrkErr_NotStarted, "stop 后 pump_once -> NotStarted");

    ork_host_destroy(host);
    CHECK(1, "destroy(host) 正常返回");
    ork_host_destroy(NULL);
    CHECK(1, "destroy(NULL) 再次调用安全");

    /* ---- 9. 本测试不应触发任何回调（从未有客户端连入） -------------------- */
    printf("\n[9] 回调未被误触发\n");
    CHECK_EQ(g_cb_connected,    0u, "on_connected 未触发");
    CHECK_EQ(g_cb_disconnected, 0u, "on_disconnected 未触发");
    CHECK_EQ(g_cb_payload,      0u, "on_payload 未触发");

    /* ---- 汇总 ----------------------------------------------------------- */
    printf("\n=============================================================\n");
    if (g_failures == 0)
    {
        printf("PASS: test_c_abi_host  (%d/%d checks)\n", g_checks - g_failures, g_checks);
        printf("=============================================================\n");
        return 0;
    }
    printf("FAIL: test_c_abi_host  (%d failures / %d checks)\n", g_failures, g_checks);
    printf("=============================================================\n");
    return 1;
}
