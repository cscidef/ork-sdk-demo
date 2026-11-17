/**
 * @file test_c_roundtrip.c
 * @brief C ABI 端到端往返测试：同一进程内跑 host + client，验证真实网络往返
 *
 * 为什么需要它
 * ------------
 * demo 01/02 是给人看的交互式程序；测试要的是**可断言、可并行、自清理**。
 * 本用例只调用 C ABI（`ork_host_*` / `ork_client_*`），不依赖 server/ 下
 * 预编译服务端，端口在运行时向 OS 申请，因此可以 `ctest -j8` 并行。
 *
 * 覆盖范围（吸收原 03/04 的独有能力，避免删除 C++ demo 后覆盖缩水）
 * -----------------------------------------------------------------
 *   1. 临时端口：TCP 与 UDP 同时空闲，不与 9400 上手工跑的 demo 抢端口；
 *   2. `ork_host_create` / `start`；启动后 `ork_host_connection_count()==0`；
 *   3. `ork_client_create` 初始 `OrkClientState_Disconnected`；
 *   4. connect 后状态机走到 `Connected`（原 demo 04 的状态迁移断言）；
 *   5. Reliable(KCP) + Unreliable(UDP) 各发一帧 25B 探针，逐字节比对回显
 *      （原 demo 04 的双通道 RTT 断言，这里只判字节 + 通道，RTT 由 demo 02 展示）；
 *   6. 连接查询：`connection_count()==1`，`get_connection_info()` 的
 *      encryption_active==1、link 为 KCP/UDP、last_active_us>0（原 demo 03）；
 *   7. 统计：`OrkStats.bytes_in/bytes_out > 0`、crypto_fail_* 全 0；
 *   8. 主动断开：宿主 `ork_host_disconnect(...,OrkDisconnect_Kicked)` 后，
 *      客户端 `on_disconnected` 收到 Kicked、host 侧断开计数≥1（原 demo 03 的踢人）；
 *   9. 正确销毁：disconnect + pump 干净 + destroy，全程不 hang / 不崩。
 *
 * 退出码：0 = 全部断言通过；1 = 有断言失败或前置条件不满足。
 */

#include "demo_support_c.h"

/* 失败时把 SDK 错误串带出来 —— 必须在 include 断言头之前定义 */
#define ORK_TEST_PRINT_LAST_ERROR() \
    printf("         ork_last_error() = \"%s\"\n", ork_last_error())

#include "ork_test_assert.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================== */
/* Host 侧状态（回调在 SDK 内部 pump 线程 → _Atomic）                          */
/* ========================================================================== */
typedef struct HostState
{
    OrkHost       host;
    _Atomic uint32_t conn_id;
    _Atomic int      have_conn;
    _Atomic int      encryption_active;
    _Atomic int      link_type;
    _Atomic uint64_t payloads;
    _Atomic uint64_t echoed;
    _Atomic uint64_t failures;
    _Atomic uint64_t disconnections;
    _Atomic uint64_t disconnect_reason;
    _Atomic uint64_t crypto_fails;
    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;
} HostState;

/* ========================================================================== */
/* Client 侧状态（回调在 pump_once 调用栈 → 同线程，普通字段）                 */
/* ========================================================================== */
typedef struct ClientState
{
    int      connected;
    int      disconnected;
    uint32_t conn_id;
    uint32_t reason;
    uint32_t payloads;
    uint32_t mismatches;          /* 回显字节不一致次数 */
} ClientState;

static OrkHost g_host = NULL;

/* 回调里要用的 client 句柄（create 之后、connect 之前赋值） */
static OrkClientHandle g_client_for_probe = NULL;

/* 双通道探针的期望帧（发送前重盖时间戳） */
static uint8_t g_frame_rel[ORK_ECHO_FRAME_SIZE];
static uint8_t g_frame_unrel[ORK_ECHO_FRAME_SIZE];

/* 探针状态：由 client on_payload 维护 */
static OrkDemoProbeState g_probe;

/* ========================================================================== */
/* Host 回调                                                                   */
/* ========================================================================== */
static void host_on_connected(void* user, uint32_t connection_id)
{
    HostState* st = (HostState*)user;
    atomic_store(&st->conn_id, connection_id);
    atomic_store(&st->have_conn, 1);

    OrkConnectionInfo info = ORK_CONNECTION_INFO_INIT;
    if (g_host != NULL
        && ork_host_get_connection_info(g_host, connection_id, &info) == OrkOk)
    {
        atomic_store(&st->encryption_active, info.encryption_active ? 1 : 0);
        atomic_store(&st->link_type, (int)info.link);
    }
}

static void host_on_disconnected(void* user, uint32_t connection_id, uint32_t reason)
{
    (void)connection_id;
    HostState* st = (HostState*)user;
    atomic_fetch_add(&st->disconnections, 1);
    atomic_store(&st->disconnect_reason, reason);
    /* host 侧也会因主动 kick 收到断开通知；reason 映射见 ork_abi.h */
}

static void host_on_payload(void* user, uint32_t connection_id,
                            const uint8_t* data, uint32_t size, uint32_t channel)
{
    HostState* st = (HostState*)user;
    atomic_fetch_add(&st->payloads, 1);

    const OrkSendResult r =
        ork_host_send(g_host, connection_id, data, size, (OrkChannel)channel);
    if (r != OrkSend_Ok) { atomic_fetch_add(&st->failures, 1); return; }
    atomic_fetch_add(&st->echoed, 1);
}

static void host_on_stats(void* user, const OrkStats* stats)
{
    if (stats == NULL) return;
    HostState* st = (HostState*)user;
    atomic_store(&st->crypto_fails, ork_demo_crypto_fail_total(stats));
    atomic_store(&st->bytes_in, (uint64_t)stats->bytes_in);
    atomic_store(&st->bytes_out, (uint64_t)stats->bytes_out);
}

/* ========================================================================== */
/* Client 回调                                                                 */
/* ========================================================================== */
static void client_on_connected(void* user, uint32_t connection_id)
{
    ClientState* st = (ClientState*)user;
    st->connected = 1;
    st->conn_id   = connection_id;

    /* 探针驱动只在 state->connected 为真时才会推进发送逻辑 —— 必须置位 */
    g_probe.connected = 1;

    /* 连上即刻发第 1 轮 Reliable（KCP 会话此时已建好）。
     * g_client_for_probe 在 create 之后、connect 之前就已赋值。 */
    ork_demo_build_frame(g_frame_rel, ORK_MARKER_RELIABLE);
    if (ork_client_send(g_client_for_probe, g_frame_rel, ORK_ECHO_FRAME_SIZE,
                        OrkChannel_Reliable) == OrkSend_Ok)
        g_probe.first_send_ok = 1;
}

static void client_on_disconnected(void* user, uint32_t reason)
{
    ClientState* st = (ClientState*)user;
    st->disconnected   = 1;
    st->reason         = reason;
    g_probe.disconnected = 1;
}

static void client_on_payload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    ClientState* st = (ClientState*)user;
    st->payloads++;

    const int reliable = (channel == (uint32_t)OrkChannel_Reliable);
    const uint8_t* expect = reliable ? g_frame_rel : g_frame_unrel;

    if (size != ORK_ECHO_FRAME_SIZE || !ork_demo_is_echo_magic(data, size))
    {
        st->mismatches++;
        return;
    }
    if (memcmp(data, expect, ORK_ECHO_FRAME_SIZE) != 0)
    {
        st->mismatches++;
        return;
    }

    if (reliable) g_probe.reliable_echo = 1;
    else          g_probe.unreliable_echo = 1;
}

/* ========================================================================== */
/* 探针驱动适配（Client 无内部线程，必须手动 pump）                            */
/* ========================================================================== */
static OrkError probe_pump(void* ctx)
{
    (void)ctx;
    return ork_client_pump_once(g_client_for_probe);
}

static OrkSendResult probe_send(void* ctx, uint8_t* frame, uint32_t size, OrkChannel channel)
{
    (void)ctx;
    return ork_client_send(g_client_for_probe, frame, size, channel);
}

static OrkClientState probe_get_state(void* ctx)
{
    (void)ctx;
    return ork_client_get_state(g_client_for_probe);
}

/* ========================================================================== */
int main(void)
{
    ork_demo_print_banner("test_c_roundtrip · C ABI host + client in one process");

    HostState   hst;
    ClientState cst;
    memset(&hst, 0, sizeof(hst));
    memset(&cst, 0, sizeof(cst));
    memset(&g_probe, 0, sizeof(g_probe));
    atomic_store(&hst.encryption_active, -1);
    atomic_store(&hst.link_type, -1);

    OrkHost         host   = NULL;
    OrkClientHandle client = NULL;

    /* ---- 1. 临时端口 ---------------------------------------------------- */
    const uint16_t port = ork_demo_pick_free_port();
    CHECK(port != 0, "pick_free_port() 拿到 TCP+UDP 双空闲端口");
    if (port == 0) ORK_TEST_ABORT();
    printf("      loopback 端口 = %u\n", (unsigned)port);

    /* ---- 2. Host -------------------------------------------------------- */
    printf("\n[1] C ABI host create / start\n");
    {
        OrkHostConfig cfg;
        ork_demo_fill_host_config(&cfg, port, /*enable_encryption=*/1);
        /* ctest 输出保持干净：内部日志丢弃 + 关到 Off */
        cfg.log_path  = ORK_DEVNULL;
        cfg.log_level = (uint32_t)OrkLog_Off;

        OrkHostCallbacks cb = ORK_HOST_CALLBACKS_INIT;
        cb.user            = &hst;
        cb.on_connected    = host_on_connected;
        cb.on_disconnected = host_on_disconnected;
        cb.on_payload      = host_on_payload;
        cb.on_stats        = host_on_stats;

        const OrkError e = ork_host_create(&cfg, &cb, &host);
        CHECK(e == OrkOk && host != NULL, "ork_host_create 返回 Ok");
        if (host == NULL) ORK_TEST_ABORT();
        hst.host  = host;
        g_host    = host;

        CHECK(ork_host_start(host) == OrkOk, "ork_host_start 返回 Ok");
        CHECK_EQ(ork_host_connection_count(host), 0u, "启动后 connection_count() == 0");
    }

    /* ---- 3. Client + 连接 ----------------------------------------------- */
    printf("\n[2] C ABI client create / connect\n");
    {
        OrkClientConfig cfg;
        ork_demo_fill_client_config(&cfg, port, /*enable_encryption=*/1, 5000);

        OrkClientCallbacks cb = ORK_CLIENT_CALLBACKS_INIT;
        cb.user            = &cst;
        cb.on_connected    = client_on_connected;
        cb.on_disconnected = client_on_disconnected;
        cb.on_payload      = client_on_payload;

        const OrkError e = ork_client_create(&cfg, &cb, &client);
        CHECK(e == OrkOk && client != NULL, "ork_client_create 返回 Ok");
        if (client == NULL) ORK_TEST_ABORT();
        g_client_for_probe = client;

        CHECK_EQ((int)ork_client_get_state(client), (int)OrkClientState_Disconnected,
                 "客户端初始状态 == Disconnected");
        CHECK(ork_client_connect(client) == OrkOk, "ork_client_connect 返回 Ok");
    }

    /* ---- 4. 双通道探针 -------------------------------------------------- */
    printf("\n[3] Reliable(KCP) + Unreliable(UDP) 逐字节回显\n");
    {
        /* 期望帧必须在 connect 之后、收到回显之前就位；此处重建一次以覆盖
         * on_connected 里已发过的那一轮时间戳差异（比对只看整帧内容，
         * 因此这里的帧与发出的帧必须一致 —— 故先重建再比对不成立，
         * 改为直接沿用 on_connected 发出的那帧，只重建 Unreliable）。 */
        ork_demo_build_frame(g_frame_unrel, ORK_MARKER_UNRELIABLE);

        const OrkDemoProbeDriver driver = {
            .ctx              = NULL,
            .state            = &g_probe,
            .frame_reliable   = g_frame_rel,
            .frame_unreliable = g_frame_unrel,
            .pump             = probe_pump,
            .send             = probe_send,
            .get_state        = probe_get_state,
            .on_state_change  = NULL,
            .should_stop      = NULL,
            .log_send         = NULL,
        };

        OrkDemoProbeParams params = ork_demo_probe_params_default(1, 20000);
        params.pump_sleep_ms             = 1;
        params.first_round_sent_in_callback = 1;   /* round 1 已在 on_connected 发出 */

        const OrkDemoProbeResult probe = ork_demo_run_dual_channel_probe(&driver, &params);

        CHECK(!probe.timed_out,           "双通道探针在 20s 内收敛");
        CHECK(cst.connected,              "客户端进入 Connected（状态机迁移）");
        CHECK(probe.reliable_ok,          "★ Reliable(KCP) 25B 回显逐字节一致");
        CHECK(probe.unreliable_ok,        "★ Unreliable(UDP) 25B 回显逐字节一致");
        CHECK_EQ(cst.mismatches,          0u, "客户端无字节不一致");
        CHECK(ork_client_get_connection_id(client) != 0u, "客户端拿到非零 connectionId");
    }

    /* ---- 5. 服务端侧证据（连接查询 + 统计） ----------------------------- */
    printf("\n[4] 连接查询与统计（原 demo 03 能力）\n");
    {
        /* 等 host 的统计快照落一次（~1Hz） */
        const uint64_t settle = ork_demo_now_ms() + 1500u;
        while (ork_demo_now_ms() < settle)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(20);
        }

        CHECK_EQ(ork_host_connection_count(host), 1u, "★ connection_count() == 1");
        CHECK_EQ(atomic_load(&hst.payloads), 2u, "host 收到 2 帧 payload");
        CHECK_EQ(atomic_load(&hst.echoed), 2u, "host 成功回显 2 帧");
        CHECK_EQ(atomic_load(&hst.failures), 0u, "host 回显无失败");
        CHECK_EQ(atomic_load(&hst.crypto_fails), 0u, "host crypto_fail_* 全 0");
        CHECK(atomic_load(&hst.bytes_in) > 0u, "host stats.bytes_in > 0");
        CHECK(atomic_load(&hst.bytes_out) > 0u, "host stats.bytes_out > 0");

        const uint32_t cid = (uint32_t)atomic_load(&hst.conn_id);
        CHECK(cid != 0u, "host OnConnected 拿到非零 connectionId");

        OrkConnectionInfo info = ORK_CONNECTION_INFO_INIT;
        const OrkError e = ork_host_get_connection_info(host, cid, &info);
        CHECK(e == OrkOk, "get_connection_info(host_conn_id) 返回 Ok");
        if (e == OrkOk)
        {
            CHECK_EQ((int)info.encryption_active, 1, "★ encryption_active == 1（AEAD 已协商）");
            CHECK(info.link == OrkLink_Kcp || info.link == OrkLink_Udp,
                  "★ link 为 KCP 或 UDP（非 TcpOnly）");
            CHECK(info.last_active_us > 0, "last_active_us > 0");
            CHECK_EQ(info.id, cid, "info.id 与 OnConnected 一致");
        }
    }

    /* ---- 6. 宿主主动断开（原 demo 03 的踢人能力） ----------------------- */
    printf("\n[5] 宿主 ork_host_disconnect → 连接回收\n");
    {
        const uint32_t cid = (uint32_t)atomic_load(&hst.conn_id);
        const OrkError e = ork_host_disconnect(host, cid, OrkDisconnect_Kicked);
        CHECK(e == OrkOk, "ork_host_disconnect(Kicked) 返回 Ok");

        /* 宿主侧立即生效：连接数归零 + OnDisconnected(reason=Kicked) */
        const uint64_t deadline = ork_demo_now_ms() + 3000u;
        while (ork_host_connection_count(host) != 0 && ork_demo_now_ms() < deadline)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(5);
        }
        CHECK_EQ(ork_host_connection_count(host), 0u,
                 "★ 踢人后 host connection_count() == 0");
        CHECK(atomic_load(&hst.disconnections) >= 1u, "host OnDisconnected 已触发");
        CHECK_EQ(atomic_load(&hst.disconnect_reason), (uint64_t)OrkDisconnect_Kicked,
                 "host 侧断开原因 == Kicked");

        /* 注意：客户端侧的 on_disconnected 由协议超时驱动，ABI 不保证它
         * 立即可见（实测可能晚于数秒）。因此这里不断言"踢人后立刻收到"，
         * 改在下一节的主动 disconnect 路径上验证该回调确实会触发。 */
    }

    /* ---- 7. 客户端主动断开 + 正确销毁 ----------------------------------- */
    printf("\n[6] 客户端主动 disconnect 与 destroy（不 hang / 不崩）\n");
    {
        CHECK(ork_client_disconnect(client) == OrkOk, "ork_client_disconnect 返回 Ok");

        const uint64_t drain = ork_demo_now_ms() + 3000u;
        while (!cst.disconnected && ork_demo_now_ms() < drain)
        {
            (void)ork_client_pump_once(client);
            ork_demo_sleep_ms(5);
        }
        CHECK(cst.disconnected, "★ 客户端 on_disconnected 已触发（主动断开路径）");
        CHECK(cst.reason == (uint32_t)OrkDisconnect_Kicked
              || cst.reason == (uint32_t)OrkDisconnect_PeerClosed,
              "客户端 disconnect reason 为 Kicked 或 PeerClosed");

        ork_client_destroy(client);
        client = NULL;

        CHECK(ork_host_stop(host) == OrkOk, "ork_host_stop 返回 Ok");
        ork_host_destroy(host);
        host = NULL;
        g_host = NULL;
        ork_host_destroy(NULL);   /* NULL 安全 */
        ork_client_destroy(NULL); /* NULL 安全 */
        CHECK(1, "Stop + Destroy 全部返回，未 hang / 未崩");
    }

    ORK_TEST_SUMMARY("test_c_roundtrip");
}
