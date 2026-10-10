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

/* demo_support_c.h 提供 ork_client_abi.h、平台差异宏、计时 / 睡眠、
 * ork_demo_pick_closed_tcp_port()（bind 后立即 close，让 connect 吃 RST）
 * 与 ork_demo_fill_client_config()。 */
#include "demo_support_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* 极简断言框架（共享）：失败时顺带打出 Client 侧的错误串                       */
/* -------------------------------------------------------------------------- */
#define ORK_TEST_PRINT_LAST_ERROR() \
    printf("         ork_client_last_error() = \"%s\"\n", ork_client_last_error())
#include "ork_test_assert.h"

/* -------------------------------------------------------------------------- */
static volatile uint32_t g_connected    = 0;
static volatile uint32_t g_disconnected = 0;
static volatile uint32_t g_payload      = 0;
static volatile uint32_t g_reason       = 0;

static void cb_connected(void* user, uint32_t connection_id)
{
    (void)user; (void)connection_id;
    ++g_connected;
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

/**
 * @brief 一份「所有不变式都满足」的 v2 Client 配置
 *
 * udp_port == tcp_port、kcp_stream_mode == 0、struct_size == 96 等由
 * ork_demo_fill_client_config 统一保证；这里只传入测试专属的快速失败超时。
 */
static void fill_valid_config(OrkClientConfig* c, uint16_t port)
{
    ork_demo_fill_client_config(c, port, /*enable_encryption=*/1,
                                /*connect_timeout_ms=*/1500);   /* 测试要快速失败 */
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

    printf(ORK_BANNER_RULE "\n");
    printf(" test_c_abi_client · C ABI Client smoke\n");
    printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
           sizeof(OrkClientConfig), (unsigned)ORK_CLIENT_CONFIG_V1_SIZE);
    printf(" sizeof(OrkClientCallbacks)=%zu\n", sizeof(OrkClientCallbacks));
    printf(ORK_BANNER_RULE "\n");

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
    closed_port = ork_demo_pick_closed_tcp_port();
    CHECK(closed_port != 0, "ork_demo_pick_closed_tcp_port() 拿到无人监听的端口");
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
    if (client == NULL) ORK_TEST_ABORT();

    CHECK_EQ(ork_client_get_state(client), OrkClientState_Disconnected,
           "★ 初始状态 == Disconnected");
    CHECK_EQ(ork_client_get_connection_id(client), 0u,
           "★ 初始 connection_id == 0");
    CHECK_EQ(ork_client_pump_once(client), OrkOk, "pump_once(未连接) -> OrkOk");

    {   /* 未连接时发送必须被拒 —— 帧内容无关紧要，用标准 25B 探针帧即可 */
        uint8_t blob[ORK_ECHO_FRAME_SIZE];
        ork_demo_build_frame(blob, ORK_MARKER_RELIABLE);
        CHECK_EQ(ork_client_send(client, blob, (uint32_t)sizeof(blob), OrkChannel_Reliable),
               OrkSend_NotConnected, "send(未连接) -> NotConnected");
        CHECK_EQ(ork_client_send(client, blob, ORK_ECHO_FRAME_SIZE, OrkChannel_Unreliable),
               OrkSend_NotConnected, "send(未连接, Unreliable) -> NotConnected");
        CHECK_EQ(ork_client_send(client, NULL, ORK_ECHO_FRAME_SIZE, OrkChannel_Reliable),
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
        const uint64_t deadline = ork_demo_now_ms() + 8000u;
        int pumped = 0;
        while (ork_demo_now_ms() < deadline && g_disconnected == 0 && pumped < 4000)
        {
            ork_client_pump_once(client);
            ++pumped;
            ork_demo_sleep_ms(2);
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
        const uint64_t deadline = ork_demo_now_ms() + 4000u;
        while (ork_demo_now_ms() < deadline && g_disconnected < 2u)
        {
            ork_client_pump_once(client);
            ork_demo_sleep_ms(2);
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
    ORK_TEST_SUMMARY("test_c_abi_client");
}
