/**
 * @file main.cpp
 * @brief Demo 06 —— C ABI 加密：AEAD 密钥协商与透明收发
 *
 * 展示内容
 * --------
 * 只调用 **C ABI**（`ork_host_*` + `ork_client_*`），在同一个进程里跑两轮
 * 完整的 host + client 配对，用可观测证据说明"加密是怎么打开的"：
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
 * ★ Bootstrap 通道本身的安全 ≠ 数据通道的加密。本 demo（以及 C ABI Host
 *   恒定使用的 PlaintextDevelopment 模式）里 bootstrap 走 **明文 TCP**，
 *   所以 SDK 会被强制 bind 到 loopback，并在启动时打一条 critical 告警。
 *   生产环境请用 C++ SDK（demo 03）配 `TlsServerAuthenticated` + TLS 1.3 证书。
 *
 * 为什么是 main.cpp 而不是 main.c
 * --------------------------------
 * Ork 的 C ABI 本身是纯 C11 的（demo 01 / 02 就是 main.c）。本 demo 用 .cpp
 * 只是为了复用 demos/common/demo_support.hpp 里的 PickFreePort()（需要 asio
 * 才能跨平台地同时探测 TCP + UDP 空闲端口）、双通道探针循环
 * RunDualChannelProbe() 与时间小工具 —— **所有 Ork API 调用都是 C ABI**，
 * 没有一个 C++ SDK 类型。
 *
 * 注意 C++ 里不能用 `ORK_HOST_CONFIG_INIT` 这类 C99 复合字面量宏
 * （`CMAKE_CXX_EXTENSIONS OFF` 下非法），改成 `OrkHostConfig c{};`
 * + 调 demo_support_c.h 的 `ork_demo_fill_host_config()` 填字段。
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

#include "demo_support.hpp"          // PickFreePort / NowMs / SleepMs / BuildEchoFrame / RunDualChannelProbe

#include "ork/sdk/c/ork_abi.h"       // ← C ABI Host（13 个函数）
#include "ork/sdk/c/ork_client_abi.h"// ← C ABI Client（9 个函数）

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

// ---------------------------------------------------------------------------
// 回调上下文（通过 OrkHostCallbacks.user / OrkClientCallbacks.user 透传）
// ---------------------------------------------------------------------------
struct HostCtx
{
    OrkHost                    host{nullptr};   // 回调里也要用
    std::atomic<uint32_t>      connId{0};
    std::atomic<bool>          haveConn{false};
    std::atomic<int>           encryptionActive{-1};  // -1=未知 0=off 1=on
    std::atomic<int>           linkType{-1};
    std::atomic<uint32_t>      payloads{0};
    std::atomic<uint32_t>      echoed{0};
    std::atomic<uint32_t>      sendFailures{0};
    std::atomic<uint32_t>      disconnections{0};
    std::atomic<unsigned long long> cryptoFails{0};
    std::atomic<unsigned long long> bytesIn{0};
    std::atomic<unsigned long long> bytesOut{0};
};

struct ClientCtx
{
    std::atomic<bool>          connected{false};
    std::atomic<bool>          disconnected{false};
    std::atomic<uint32_t>      connId{0};
    std::atomic<uint32_t>      reason{0};
    std::atomic<uint32_t>      payloads{0};
    std::atomic<bool>          reliableEcho{false};
    std::atomic<bool>          unreliableEcho{false};
    std::atomic<bool>          mismatch{false};

    // 期望帧：主线程在 Connect 之前填好；on_payload 在同一线程（pump_once
    // 调用栈内）读取，无需额外同步。
    std::array<uint8_t, orkdemo::kEchoFrameSize> frameReliable{};
    std::array<uint8_t, orkdemo::kEchoFrameSize> frameUnreliable{};
};

// ---------------------------------------------------------------------------
// 小工具
//
// 枚举名映射（SendResultName / ClientStateName / LinkName）、日志级别解析
// （EnvLogLevel）与 crypto_fail_* 求和（CryptoFailTotal）都来自共享头，
// 本文件不再各留一份（F1 / F9）。
// ---------------------------------------------------------------------------
/** @brief 取 25B 帧的 marker（8 字节，不保证 NUL 结尾） */
std::string_view FrameMarker(const uint8_t* data, uint32_t size) noexcept
{
    const char* m = ork_demo_frame_marker(data, size);
    return (m != nullptr) ? std::string_view(m, ORK_MARKER_LEN) : std::string_view{};
}

// ---------------------------------------------------------------------------
// Host 回调（在 core 内部 pump 线程上触发）
//
// 线程契约：send / broadcast / disconnect 在回调里安全（只入队 / 异步 close）；
// stop / destroy 绝对不行（要 join 当前线程 → 自死锁）。
// ---------------------------------------------------------------------------
void HostOnConnected(void* user, uint32_t connectionId)
{
    auto* ctx = static_cast<HostCtx*>(user);
    ctx->connId.store(connectionId);
    ctx->haveConn.store(true);

    // 这就是"cipher 已在 bootstrap 阶段协商完成"的服务端侧证据
    OrkConnectionInfo info{};
    info.struct_size = sizeof(OrkConnectionInfo);
    if (ctx->host != nullptr
        && ork_host_get_connection_info(ctx->host, connectionId, &info) == OrkOk)
    {
        ctx->encryptionActive.store(info.encryption_active ? 1 : 0);
        ctx->linkType.store(static_cast<int>(info.link));
        std::printf("[host] + connection %u  link=%s  encryption_active=%d  <<< bootstrap 协商结果\n",
                    connectionId, orkdemo::LinkName(info.link),
                    static_cast<int>(info.encryption_active));
    }
    else
    {
        std::printf("[host] + connection %u  (get_connection_info 失败)\n", connectionId);
    }
    std::fflush(stdout);
}

void HostOnDisconnected(void* user, uint32_t connectionId, uint32_t reason)
{
    auto* ctx = static_cast<HostCtx*>(user);
    ctx->disconnections.fetch_add(1);
    std::printf("[host] - connection %u  reason=%u\n", connectionId, reason);
    std::fflush(stdout);
}

void HostOnPayload(void* user, uint32_t connectionId,
                   const uint8_t* data, uint32_t size, uint32_t channel)
{
    auto* ctx = static_cast<HostCtx*>(user);
    ctx->payloads.fetch_add(1);

    // 原样、同通道回显。data 的生命周期仅限本次回调。
    const OrkSendResult r =
        ork_host_send(ctx->host, connectionId, data, size, static_cast<OrkChannel>(channel));
    if (r != OrkSend_Ok)
    {
        ctx->sendFailures.fetch_add(1);
        std::printf("[host] ! echo to %u failed: %s\n", connectionId, orkdemo::SendResultName(r));
        std::fflush(stdout);
        return;
    }
    ctx->echoed.fetch_add(1);

    const std::string_view marker = FrameMarker(data, size);
    if (!marker.empty())
    {
        std::printf("[host] ~ echo %u B to %u via %s (marker=%.*s)\n",
                    size, connectionId,
                    channel == static_cast<uint32_t>(OrkChannel_Reliable) ? "Reliable"
                                                                          : "Unreliable",
                    static_cast<int>(marker.size()), marker.data());
        std::fflush(stdout);
    }
}

void HostOnStats(void* user, const OrkStats* stats)
{
    if (stats == nullptr) return;
    auto* ctx = static_cast<HostCtx*>(user);
    // 加密失败四计数器：任何一项非 0 都说明 AEAD 协商/密钥出了问题
    ctx->cryptoFails.store(orkdemo::CryptoFailTotal(*stats));   // F9
    ctx->bytesIn.store(static_cast<unsigned long long>(stats->bytes_in));
    ctx->bytesOut.store(static_cast<unsigned long long>(stats->bytes_out));
}

// ---------------------------------------------------------------------------
// Client 回调（在主线程 ork_client_pump_once / disconnect 的调用栈内触发）
// ---------------------------------------------------------------------------
void ClientOnConnected(void* user, uint32_t connectionId)
{
    auto* ctx = static_cast<ClientCtx*>(user);
    ctx->connId.store(connectionId);
    ctx->connected.store(true);
    std::printf("[client] OnConnected connectionId=%u -> 立刻发两通道探针\n", connectionId);
    std::fflush(stdout);
}

void ClientOnDisconnected(void* user, uint32_t reason)
{
    auto* ctx = static_cast<ClientCtx*>(user);
    ctx->reason.store(reason);
    ctx->disconnected.store(true);
}

void ClientOnPayload(void* user, const uint8_t* data, uint32_t size, uint32_t channel)
{
    auto* ctx = static_cast<ClientCtx*>(user);
    ctx->payloads.fetch_add(1);

    const bool reliable = (channel == static_cast<uint32_t>(OrkChannel_Reliable));
    const auto& expect = reliable ? ctx->frameReliable : ctx->frameUnreliable;

    if (size != expect.size() || std::memcmp(data, expect.data(), expect.size()) != 0)
    {
        ctx->mismatch.store(true);
        std::fprintf(stderr, "[client] ! %s 回显字节不一致 (got %u B, 期望 %zu B)\n",
                     reliable ? "Reliable" : "Unreliable", size, expect.size());
        return;
    }

    if (reliable) ctx->reliableEcho.store(true);
    else          ctx->unreliableEcho.store(true);

    std::printf("[client] <- %s 回显逐字节一致 (%u B) —— 加解密对业务层透明\n",
                reliable ? "Reliable(KCP)  " : "Unreliable(UDP)", size);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 单轮结果
// ---------------------------------------------------------------------------
struct RoundResult
{
    bool     ok            = false;
    bool     hostCreated   = false;
    bool     clientCreated = false;
    bool     connected     = false;
    bool     reliableEcho  = false;
    bool     unreliableEcho= false;
    int      encryptionActive = -1;
    int      linkType      = -1;
    unsigned long long cryptoFails = 0;
    unsigned long long bytesIn  = 0;
    unsigned long long bytesOut = 0;
    uint32_t hostConnId    = 0;
    uint32_t clientConnId  = 0;
    std::string error;
};

// ---------------------------------------------------------------------------
// 拆除（NULL 安全；错误路径与正常路径共用）
// ---------------------------------------------------------------------------
/** @brief 拆 host：stop + destroy。未 start 就 stop 只会得到 NotStarted，无害。 */
void DestroyHost(OrkHost host)
{
    if (host == nullptr) return;
    ork_host_stop(host);          // 不得在网络 pump 线程内调用（这里在主线程，OK）
    ork_host_destroy(host);
}

/** @brief 拆 client：disconnect + 把异步收尾 pump 干净 + destroy */
void DestroyClient(OrkClientHandle client, ClientCtx& cctx)
{
    if (client == nullptr) return;
    ork_client_disconnect(client);
    const double drainUntil = orkdemo::NowMs() + 2000.0;
    while (orkdemo::NowMs() < drainUntil && !cctx.disconnected.load())
    {
        ork_client_pump_once(client);
        orkdemo::SleepMs(10);
    }
    ork_client_destroy(client);
}

// ===========================================================================
// F25：RunRound 原本是一个 244 行的巨函数，按既有的 `// ---- N. xxx ----`
// 小节边界拆成 5 个各司其职的函数，RunRound 只剩「编排 + 判定」。
// ===========================================================================

// ---- 1. Host --------------------------------------------------------------
/**
 * @brief 建 host 并 Start
 * @param[out] outHost  成功时写入句柄；失败时保持 nullptr
 * @return false 表示已把错误写进 res.error 并完成必要清理
 */
bool CreateHost(uint16_t port, bool encryption, HostCtx& hctx,
                OrkHost& outHost, RoundResult& res)
{
    // 默认值与全部不变式（tcp_port == udp_port、drive_mode == 0、
    // kcp_update_rate_hz >= pump_rate_hz、max_packet_size ∈ [50,1414] …）
    // 由 demo_support_c.h 的 ork_demo_fill_host_config 统一给出（F8），
    // 这里只覆盖两个跟环境变量走的日志字段。
    OrkHostConfig hcfg{};                       // C++ 里不能用 ORK_HOST_CONFIG_INIT
    ork_demo_fill_host_config(&hcfg, port, encryption ? 1 : 0);
    hcfg.log_path  = ork_demo_env_log_file();
    hcfg.log_level = orkdemo::EnvLogLevel("demo");

    OrkHostCallbacks hcb{};
    hcb.struct_size     = sizeof(OrkHostCallbacks);
    hcb.user            = &hctx;
    hcb.on_connected    = &HostOnConnected;
    hcb.on_disconnected = &HostOnDisconnected;
    hcb.on_payload      = &HostOnPayload;
    hcb.on_stats        = &HostOnStats;

    OrkHost host = nullptr;
    OrkError e = ork_host_create(&hcfg, &hcb, &host);
    if (e != OrkOk || host == nullptr)
    {
        res.error = std::string("ork_host_create: ") + ork_last_error();
        std::fprintf(stderr, "[round] %s\n", res.error.c_str());
        return false;
    }
    res.hostCreated = true;
    hctx.host = host;                 // 回调里要用（get_connection_info / send）

    e = ork_host_start(host);
    if (e != OrkOk)
    {
        res.error = std::string("ork_host_start: ") + ork_last_error();
        std::fprintf(stderr, "[round] %s\n", res.error.c_str());
        DestroyHost(host);
        return false;
    }
    std::printf("[host] started  sizeof(OrkHostConfig)=%zu  sizeof(OrkHostCallbacks)=%zu\n",
                sizeof(OrkHostConfig), sizeof(OrkHostCallbacks));
    std::fflush(stdout);

    outHost = host;
    return true;
}

// ---- 2. Client ------------------------------------------------------------
/**
 * @brief 建 client、填期望帧、Connect
 * @param[out] outClient  成功时写入句柄；失败时调用方只需再拆 host
 */
bool CreateClient(uint16_t port, bool encryption, ClientCtx& cctx,
                  OrkClientHandle& outClient, RoundResult& res)
{
    // v2 布局（96 字节）；udp_port == tcp_port、kcp_stream_mode == 0 等
    // 不变式同样由共享填充函数保证（F8）。
    OrkClientConfig ccfg{};
    ork_demo_fill_client_config(&ccfg, port, encryption ? 1 : 0,
                                /*connect_timeout_ms=*/5000);

    OrkClientCallbacks ccb{};
    ccb.struct_size     = sizeof(OrkClientCallbacks);
    ccb.user            = &cctx;
    ccb.on_connected    = &ClientOnConnected;
    ccb.on_disconnected = &ClientOnDisconnected;
    ccb.on_payload      = &ClientOnPayload;

    OrkClientHandle client = nullptr;
    OrkError e = ork_client_create(&ccfg, &ccb, &client);
    if (e != OrkOk || client == nullptr)
    {
        res.error = std::string("ork_client_create: ") + ork_client_last_error();
        std::fprintf(stderr, "[round] %s\n", res.error.c_str());
        return false;
    }
    res.clientCreated = true;
    std::printf("[client] created  sizeof(OrkClientConfig)=%zu (v2)  "
                "ORK_CLIENT_CONFIG_V1_SIZE=%u  state=%s\n",
                sizeof(OrkClientConfig),
                static_cast<unsigned>(ORK_CLIENT_CONFIG_V1_SIZE),
                orkdemo::ClientStateName(ork_client_get_state(client)));
    std::fflush(stdout);

    // 期望帧（Connect 之前就要就位：on_connected 之后紧接着就会收到回显）
    orkdemo::BuildEchoFrame(cctx.frameReliable,   orkdemo::kMarkerReliable);
    orkdemo::BuildEchoFrame(cctx.frameUnreliable, orkdemo::kMarkerUnreliable);

    e = ork_client_connect(client);
    if (e != OrkOk)
    {
        res.error = std::string("ork_client_connect: ") + ork_client_last_error();
        std::fprintf(stderr, "[round] %s\n", res.error.c_str());
        ork_client_destroy(client);
        return false;
    }

    outClient = client;
    return true;
}

// ---- 3. 手动驱动客户端（Client 没有内部线程！）----------------------------
/**
 * 双通道探针的 C ABI Driver 适配（F2）。
 *
 * 循环节奏（先发 Reliable、Unreliable 按 150ms 重试至多 8 次、收到回显即停）
 * 由 orkdemo::RunDualChannelProbe 统一给出，与 demo 02（纯 C 函数指针版）、
 * demo 04 / test_cpp_roundtrip（C++ SDK 版）是同一套语义。
 */
struct CAbiProbeDriver
{
    OrkClientHandle client;
    ClientCtx&      ctx;
    OrkClientState  cur  = OrkClientState_Disconnected;
    OrkClientState  last = OrkClientState_Disconnected;

    bool ShouldStop() const noexcept { return false; }   // 本 demo 不响应 Ctrl+C

    // pump_once 的 OrkError 一律忽略（与改造前一致）：出错会在状态机与
    // 回显断言上暴露出来，不在这里提前退出。
    bool Pump() { ork_client_pump_once(client); return true; }

    void OnTick()
    {
        cur = ork_client_get_state(client);
        if (cur == last) return;
        std::printf("[client] state: %s -> %s\n",
                    orkdemo::ClientStateName(last), orkdemo::ClientStateName(cur));
        std::fflush(stdout);
        last = cur;
    }

    bool Connected()    const noexcept { return cur == OrkClientState_Connected; }
    bool Disconnected() const noexcept { return false; }   // 靠超时兜底
    bool ReliableEchoed()   const noexcept { return ctx.reliableEcho.load(); }
    bool UnreliableEchoed() const noexcept { return ctx.unreliableEcho.load(); }
    void ClearReliableEcho() noexcept { ctx.reliableEcho.store(false); }

    bool SendReliable(int /*round*/)
    {
        const OrkSendResult r = ork_client_send(
            client, ctx.frameReliable.data(),
            static_cast<uint32_t>(ctx.frameReliable.size()), OrkChannel_Reliable);
        std::printf("[client] -> Reliable(KCP)   %zu B send=%s\n",
                    ctx.frameReliable.size(), orkdemo::SendResultName(r));
        std::fflush(stdout);
        return r == OrkSend_Ok;
    }

    // Unreliable 走裸 UDP，握手刚完成时路由可能还没就绪：
    // 最多重试 8 次，每次间隔 150ms，收到回显即停。
    bool SendUnreliable(int attempt)
    {
        const OrkSendResult r = ork_client_send(
            client, ctx.frameUnreliable.data(),
            static_cast<uint32_t>(ctx.frameUnreliable.size()), OrkChannel_Unreliable);
        std::printf("[client] -> Unreliable(UDP) %zu B attempt=%d send=%s\n",
                    ctx.frameUnreliable.size(), attempt, orkdemo::SendResultName(r));
        std::fflush(stdout);
        return r == OrkSend_Ok;
    }
};

orkdemo::DualChannelProbeResult RunProbeLoop(OrkClientHandle client, ClientCtx& cctx,
                                            int timeoutSec)
{
    CAbiProbeDriver drv{client, cctx};
    orkdemo::DualChannelProbeParams params;
    params.reliableRounds = 1;                   // 本 demo 每轮只发一帧探针
    params.timeoutMs      = timeoutSec * 1000;
    params.pumpSleepMs    = ORK_PUMP_SLEEP_MS;   // 5ms
    return orkdemo::RunDualChannelProbe(drv, params);
}

// ---- 4/5. 等统计快照 + 收集证据 -------------------------------------------
void CollectEvidence(OrkClientHandle client, OrkHost host,
                     HostCtx& hctx, ClientCtx& cctx, RoundResult& res)
{
    // 让 OnStats 至少落一次（~1Hz），否则 crypto_fail_* 恒为 0 没意义
    std::printf("[round] 等待统计快照 (~1.3s) ...\n");
    std::fflush(stdout);
    const double statsUntil = orkdemo::NowMs() + 1300.0;
    while (orkdemo::NowMs() < statsUntil)
    {
        ork_client_pump_once(client);
        orkdemo::SleepMs(20);
    }

    OrkConnectionInfo info{};
    info.struct_size = sizeof(OrkConnectionInfo);
    if (ork_host_get_connection_info(host, cctx.connId.load(), &info) == OrkOk)
    {
        res.encryptionActive = info.encryption_active ? 1 : 0;
        res.linkType         = static_cast<int>(info.link);
    }
    else
    {
        // 连接可能已经掉了，回退用回调里缓存的值
        res.encryptionActive = hctx.encryptionActive.load();
        res.linkType         = hctx.linkType.load();
    }

    OrkStats st{};
    st.struct_size = sizeof(OrkStats);
    if (ork_host_get_stats(host, &st) == OrkOk)
    {
        res.cryptoFails = orkdemo::CryptoFailTotal(st);      // F9
        res.bytesIn  = static_cast<unsigned long long>(st.bytes_in);
        res.bytesOut = static_cast<unsigned long long>(st.bytes_out);
        std::printf("[host] stats: aead=%llu replay=%llu expired=%llu malformed=%llu "
                    "decode=%llu  bytes in/out=%llu/%llu  active=%u\n",
                    static_cast<unsigned long long>(st.crypto_fail_aead),
                    static_cast<unsigned long long>(st.crypto_fail_replay),
                    static_cast<unsigned long long>(st.crypto_fail_expired),
                    static_cast<unsigned long long>(st.crypto_fail_malformed),
                    static_cast<unsigned long long>(st.decode_failures),
                    res.bytesIn, res.bytesOut, st.active_connections);
        std::fflush(stdout);
    }

    res.connected      = cctx.connected.load();
    res.reliableEcho   = cctx.reliableEcho.load();
    res.unreliableEcho = cctx.unreliableEcho.load();
    res.hostConnId     = hctx.connId.load();
    res.clientConnId   = cctx.connId.load();
    if (cctx.mismatch.load()) res.error = "回显字节不一致";
}

// ---- 6. 拆除 --------------------------------------------------------------
void Teardown(OrkClientHandle client, OrkHost host, ClientCtx& cctx)
{
    DestroyClient(client, cctx);
    DestroyHost(host);
}

// ===========================================================================
// 一轮 A/B 对照：编排上面 5 步 + 判定
// ===========================================================================
RoundResult RunRound(const char* label, bool encryption, int timeoutSec)
{
    RoundResult res;

    std::printf("\n");
    orkdemo::PrintBannerRule();
    std::printf(" %s · enable_encryption = %d\n", label, encryption ? 1 : 0);
    orkdemo::PrintBannerRule();
    std::fflush(stdout);

    // ---- 0. 端口：TCP bootstrap 与 UDP/KCP 共用，必须两边都空闲 -----------
    const uint16_t port = orkdemo::PickFreePort();
    if (port == 0)
    {
        res.error = "PickFreePort 失败";
        std::fprintf(stderr, "[round] %s\n", res.error.c_str());
        return res;
    }
    std::printf("[round] loopback 端口 = %u (TCP + UDP 共用)\n", static_cast<unsigned>(port));
    std::fflush(stdout);

    HostCtx         hctx;
    ClientCtx       cctx;
    OrkHost         host   = nullptr;
    OrkClientHandle client = nullptr;

    if (!CreateHost(port, encryption, hctx, host, res)) return res;
    if (!CreateClient(port, encryption, cctx, client, res))
    {
        DestroyHost(host);
        return res;
    }

    const orkdemo::DualChannelProbeResult probe = RunProbeLoop(client, cctx, timeoutSec);
    if (probe.timedOut)
        std::fprintf(stderr, "[round] 双通道探针超时（%d s）\n", timeoutSec);

    CollectEvidence(client, host, hctx, cctx, res);
    Teardown(client, host, cctx);

    res.ok = res.connected && res.reliableEcho && res.unreliableEcho
          && res.cryptoFails == 0 && res.error.empty();
    return res;
}

void PrintRow(const char* label, bool expectEnc, const RoundResult& r)
{
    std::printf("  %-14s enc=%d  encryption_active=%d (期望 %d) %s  link=%s  "
                "connId=%u/%u  echo R/U=%s/%s  cryptoFail=%llu  bytes in/out=%llu/%llu  %s\n",
                label, expectEnc ? 1 : 0, r.encryptionActive, expectEnc ? 1 : 0,
                r.encryptionActive == (expectEnc ? 1 : 0) ? "OK" : "MISMATCH",
                r.linkType >= 0 ? orkdemo::LinkName(static_cast<uint32_t>(r.linkType)) : "?",
                r.hostConnId, r.clientConnId,
                r.reliableEcho ? "OK" : "FAIL",
                r.unreliableEcho ? "OK" : "FAIL",
                r.cryptoFails, r.bytesIn, r.bytesOut,
                r.ok ? "PASS" : "FAIL");
    std::fflush(stdout);
}

void Usage(const char* prog)
{
    std::printf("用法: %s [--timeout SEC] [--rounds a|b|both] [--help]\n"
                "  --timeout SEC  单轮超时（默认 15）\n"
                "  --rounds a     只跑加密轮（enable_encryption=1）\n"
                "  --rounds b     只跑对照轮（enable_encryption=0）\n"
                "  --rounds both  两轮都跑（默认）\n"
                "\n环境变量:\n"
                "  ORK_LOG_FILE   内部日志文件（默认 %s）\n"
                "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
                prog, orkdemo::kDevNull);
}

}  // namespace

int main(int argc, char** argv)
{
    int timeoutSec = 15;
    bool runA = true, runB = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
        if (a == "--timeout" && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v < 1 || v > 600) { std::fprintf(stderr, "[demo] --timeout 需在 [1,600]\n"); return 2; }
            timeoutSec = static_cast<int>(v);
            continue;
        }
        if (a == "--rounds" && i + 1 < argc)
        {
            const std::string v = argv[++i];
            if      (v == "a")    { runA = true;  runB = false; }
            else if (v == "b")    { runA = false; runB = true;  }
            else if (v == "both") { runA = true;  runB = true;  }
            else { std::fprintf(stderr, "[demo] --rounds 只接受 a|b|both\n"); return 2; }
            continue;
        }
        std::fprintf(stderr, "[demo] 未知参数 %s\n", a.c_str());
        Usage(argv[0]);
        return 2;
    }

    orkdemo::PrintBannerRule();
    std::printf(" Demo 06 · C ABI 加密：AEAD 密钥协商与透明收发\n");
    std::printf(" ORK_ABI_VERSION=%u  sizeof(OrkHostConfig)=%zu\n",
                ork_abi_version(), sizeof(OrkHostConfig));
    std::printf(" sizeof(OrkClientConfig)=%zu (v2)  ORK_CLIENT_CONFIG_V1_SIZE=%u\n",
                sizeof(OrkClientConfig), static_cast<unsigned>(ORK_CLIENT_CONFIG_V1_SIZE));
    std::printf(" cipher = ChaCha20-Poly1305-IETF（密钥来自 Bootstrap V2 帧的 c2sKey/s2cKey）\n");
    orkdemo::PrintBannerRule();
    std::fflush(stdout);

    RoundResult a, b;
    if (runA) a = RunRound("Round A (加密)",   true,  timeoutSec);
    if (runB) b = RunRound("Round B (对照·明文)", false, timeoutSec);

    std::printf("\n");
    orkdemo::PrintBanner("结果汇总");
    bool ok = true;
    if (runA) { PrintRow("Round A 加密",   true,  a); ok = ok && a.ok && a.encryptionActive == 1; }
    if (runB) { PrintRow("Round B 对照",   false, b); ok = ok && b.ok && b.encryptionActive == 0; }

    orkdemo::PrintSeparator();
    if (runA && runB)
    {
        std::printf(" 结论：encryption_active 随服务端 enable_encryption 翻转 (%d -> %d)，\n"
                    "       两轮 payload 均逐字节一致，crypto_fail_* 全 0。\n"
                    "       => AEAD 在 bootstrap 阶段协商完成，对业务层完全透明。\n",
                    a.encryptionActive, b.encryptionActive);
    }
    std::printf("%s\n", ok ? "PASS: C ABI encryption negotiation"
                           : "FAIL: 见上面的 MISMATCH / FAIL 项");
    std::fflush(stdout);
    return ok ? 0 : 1;
}
