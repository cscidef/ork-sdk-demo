/**
 * @file test_cpp_roundtrip.cpp
 * @brief C++ SDK 端到端往返测试：同一进程内跑 host + client，验证回显
 *
 * 覆盖范围（真正的网络往返，不是 mock）：
 *   1. SessionFramework::Create(TransportConfig) 成功；
 *   2. RegisterListener + Start 成功，服务端在临时端口上同时监听
 *      TCP bootstrap 与 UDP/KCP；
 *   3. ork::client::OrkClient 连上后状态机走到 Connected；
 *   4. Reliable(KCP) + Unreliable(UDP) 各发一帧 25B "ORKBENCH1" 探针，
 *      服务端 EchoListener 原样同通道回显，客户端逐字节比对通过；
 *   5. IConnectionRegistry 侧证据：Count()==1、ConnectionView.encryptionActive
 *      ==true、linkState==Established；
 *   6. host 侧 payloads==2 / echoed==2 / failures==0 / cryptoFails==0；
 *   7. Disconnect + Stop + RegisterListener(nullptr) 干净收尾（不 hang、不崩）。
 *
 * 端口在运行时向 OS 申请（TCP+UDP 同时空闲），所以可以 `ctest -j8` 并行，
 * 也不会和 9400 上手工跑的 demo 抢端口。
 *
 * 退出码：0 = 全部断言通过；1 = 有断言失败或异常。
 */

#include "demo_support.hpp"

#include "ork/client/ork_client.hpp"
#include "ork/sdk/host_listener.hpp"
#include "ork/sdk/sender.hpp"
#include "ork/sdk/sdk/session_interface.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// ============================================================================
// 极简断言框架（与两个纯 C 测试风格一致）
// ============================================================================
int g_checks   = 0;
int g_failures = 0;

void Check(bool cond, const char* label, int line)
{
    ++g_checks;
    if (cond)
    {
        std::printf("  [ok]   %s\n", label);
    }
    else
    {
        ++g_failures;
        std::printf("  [FAIL] %s   (line %d)\n", label, line);
    }
    std::fflush(stdout);
}
#define CHECK(cond, label) Check(static_cast<bool>(cond), (label), __LINE__)

void CheckEqU64(uint64_t actual, uint64_t expected, const char* label, int line)
{
    ++g_checks;
    if (actual == expected)
    {
        std::printf("  [ok]   %s (= %llu)\n", label,
                    static_cast<unsigned long long>(actual));
    }
    else
    {
        ++g_failures;
        std::printf("  [FAIL] %s: got %llu, expected %llu   (line %d)\n", label,
                    static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(expected), line);
    }
    std::fflush(stdout);
}
#define CHECK_EQ(actual, expected, label) \
    CheckEqU64(static_cast<uint64_t>(actual), static_cast<uint64_t>(expected), (label), __LINE__)

// ============================================================================
// 服务端监听器：收到什么就原样、同通道送回去（与 demo 03 的 EchoListener 同构，
// 但这里只统计，不打业务日志，保持 ctest 输出干净）
// ============================================================================
class EchoListener final : public ork::sdk::IHostListener
{
public:
    explicit EchoListener(ork::sdk::ISender& sender) noexcept : sender_(sender) {}

    void OnConnected(ConnectionId connectionId) override
    {
        lastConnectionId_.store(connectionId, std::memory_order_relaxed);
        connections_.fetch_add(1, std::memory_order_relaxed);
    }

    void OnDisconnected(ConnectionId, DisconnectReason) override
    {
        disconnections_.fetch_add(1, std::memory_order_relaxed);
    }

    void OnPayload(ConnectionId connectionId,
                   ork::sdk::Channel channel,
                   std::span<const uint8_t> payload) override
    {
        payloads_.fetch_add(1, std::memory_order_relaxed);

        // ★ 回调跑在 core 内部 pump 线程上；ISender::Send 只入队，回调内安全。
        const ork::sdk::SendResult r = sender_.Send(connectionId, payload, channel);
        if (r != ork::sdk::SendResult::Ok)
            failures_.fetch_add(1, std::memory_order_relaxed);
        else
            echoed_.fetch_add(1, std::memory_order_relaxed);
    }

    void OnStats(const ork::sdk::OrkStats& stats) override
    {
        cryptoFails_.store(stats.cryptoFailAead + stats.cryptoFailReplay
                           + stats.cryptoFailExpired + stats.cryptoFailMalformed,
                           std::memory_order_relaxed);
        bytesIn_.store(stats.bytesIn, std::memory_order_relaxed);
        bytesOut_.store(stats.bytesOut, std::memory_order_relaxed);
        statsSeen_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t connections()      const noexcept { return connections_.load(std::memory_order_relaxed); }
    uint64_t disconnections()   const noexcept { return disconnections_.load(std::memory_order_relaxed); }
    uint64_t payloads()         const noexcept { return payloads_.load(std::memory_order_relaxed); }
    uint64_t echoed()           const noexcept { return echoed_.load(std::memory_order_relaxed); }
    uint64_t failures()         const noexcept { return failures_.load(std::memory_order_relaxed); }
    uint64_t cryptoFails()      const noexcept { return cryptoFails_.load(std::memory_order_relaxed); }
    uint64_t bytesIn()          const noexcept { return bytesIn_.load(std::memory_order_relaxed); }
    uint64_t bytesOut()         const noexcept { return bytesOut_.load(std::memory_order_relaxed); }
    uint64_t statsSeen()        const noexcept { return statsSeen_.load(std::memory_order_relaxed); }
    uint32_t lastConnectionId() const noexcept { return lastConnectionId_.load(std::memory_order_relaxed); }

private:
    ork::sdk::ISender& sender_;

    std::atomic<uint64_t> connections_{0};
    std::atomic<uint64_t> disconnections_{0};
    std::atomic<uint64_t> payloads_{0};
    std::atomic<uint64_t> echoed_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<uint64_t> cryptoFails_{0};
    std::atomic<uint64_t> bytesIn_{0};
    std::atomic<uint64_t> bytesOut_{0};
    std::atomic<uint64_t> statsSeen_{0};
    std::atomic<uint32_t> lastConnectionId_{0};
};

// ============================================================================
// 客户端监听器：记录状态迁移 + 逐字节比对回显
//
// 线程契约：OrkClient 没有内部线程，回调都在 PumpOnce()/Disconnect() 的调用栈内
// 触发（本测试全部在主线程），所以普通成员变量就够，不需要 atomic。
// ============================================================================
class RoundtripListener final : public ork::client::IClientListener
{
public:
    void OnConnected(uint32_t connectionId) override
    {
        connectionId_ = connectionId;
        connected_    = true;
        states_.emplace_back("Connected");
    }

    void OnDisconnected(uint32_t reason) override
    {
        disconnectReason_ = reason;
        disconnected_     = true;
    }

    void OnPayload(std::span<const uint8_t> data, TransportChannel channel) override
    {
        ++payloads_;
        const bool reliable = (channel == TransportChannel::Reliable);
        const std::span<const uint8_t> expect =
            reliable ? std::span<const uint8_t>(frameReliable_)
                     : std::span<const uint8_t>(frameUnreliable_);

        if (data.size() != expect.size()
            || std::memcmp(data.data(), expect.data(), expect.size()) != 0)
        {
            ++mismatches_;
            return;
        }
        if (reliable) reliableEcho_ = true; else unreliableEcho_ = true;
    }

    // ---- 主线程读写 ----
    bool     connected()        const noexcept { return connected_; }
    bool     disconnected()     const noexcept { return disconnected_; }
    bool     reliableEcho()     const noexcept { return reliableEcho_; }
    bool     unreliableEcho()   const noexcept { return unreliableEcho_; }
    uint32_t connectionId()     const noexcept { return connectionId_; }
    uint32_t disconnectReason() const noexcept { return disconnectReason_; }
    uint64_t payloads()         const noexcept { return payloads_; }
    uint64_t mismatches()       const noexcept { return mismatches_; }
    const std::vector<std::string>& states() const noexcept { return states_; }

    std::array<uint8_t, orkdemo::kEchoFrameSize> frameReliable_{};
    std::array<uint8_t, orkdemo::kEchoFrameSize> frameUnreliable_{};

private:
    bool     connected_        = false;
    bool     disconnected_     = false;
    bool     reliableEcho_     = false;
    bool     unreliableEcho_   = false;
    uint32_t connectionId_     = 0;
    uint32_t disconnectReason_ = 0;
    uint64_t payloads_         = 0;
    uint64_t mismatches_       = 0;
    std::vector<std::string> states_;
};

}  // namespace

// ============================================================================
int main()
{
    // 默认静音（内部日志级别 Off + 文件 /dev/null）。排障时：
    //   ORK_LOG_FILE=/tmp/ork.log ORK_LOG_LEVEL=debug ./test_cpp_roundtrip
    orkdemo::InitLogging("test_cpp_roundtrip", LogLevel::Off);

    std::printf("=============================================================\n");
    std::printf(" test_cpp_roundtrip · C++ SDK host + client in one process\n");
    std::printf("=============================================================\n");

    // 失败也要把 host/client 收尾干净，用 RAII + 提前 return 的组合。
    std::unique_ptr<SessionFramework> framework;
    std::unique_ptr<EchoListener>     listener;
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<ork::client::OrkClient> client;
    bool clientAlive = false;
    bool hostStarted = false;

    auto finish = [&](int code) -> int {
        if (clientAlive)
        {
            client->Disconnect();
            // 把 Disconnect 的异步收尾 pump 干净，避免析构时还有 in-flight 回调
            for (int i = 0; i < 40; ++i) { client->PumpOnce(); orkdemo::SleepMs(5); }
            client.reset();
            clientAlive = false;
        }
        io.reset();
        if (framework)
        {
            if (hostStarted) framework->Stop();
            framework->RegisterListener(nullptr);   // 先摘监听再析构
            framework.reset();
        }
        listener.reset();

        std::printf("\n=============================================================\n");
        if (code == 0)
            std::printf("PASS: test_cpp_roundtrip  (%d/%d checks)\n",
                        g_checks - g_failures, g_checks);
        else
            std::printf("FAIL: test_cpp_roundtrip  (%d failures / %d checks)\n",
                        g_failures, g_checks);
        std::printf("=============================================================\n");
        return code;
    };

    try
    {
        // ---- 1. 临时端口（TCP bootstrap 与 UDP/KCP 共用）------------------
        const uint16_t port = orkdemo::PickFreePort();
        CHECK(port != 0, "PickFreePort() 拿到 TCP+UDP 双空闲端口");
        if (port == 0) return finish(1);
        std::printf("      loopback 端口 = %u\n", static_cast<unsigned>(port));

        // ---- 2. 服务端 ---------------------------------------------------
        std::printf("\n[1] SessionFramework host\n");
        const TransportConfig hcfg = orkdemo::MakeHostConfig(port, /*encryption=*/true);
        framework = SessionFramework::Create(hcfg);
        CHECK(framework != nullptr, "SessionFramework::Create 返回非空");
        if (!framework) return finish(1);

        listener = std::make_unique<EchoListener>(framework->GetSender());
        framework->RegisterListener(listener.get());
        framework->Start();
        hostStarted = true;
        CHECK(true, "RegisterListener + Start 未抛异常");

        IConnectionRegistry& registry = framework->GetConnectionRegistry();
        CHECK_EQ(registry.Count(), 0u, "启动后 registry.Count() == 0");

        // ---- 3. 客户端 ---------------------------------------------------
        std::printf("\n[2] OrkClient 连接\n");
        io     = std::make_unique<asio::io_context>();
        client = std::make_unique<ork::client::OrkClient>(
            *io, orkdemo::MakeClientConfig("127.0.0.1", port, /*encryption=*/true));
        clientAlive = true;

        RoundtripListener cl;
        orkdemo::BuildEchoFrame(cl.frameReliable_,   orkdemo::kMarkerReliable);
        orkdemo::BuildEchoFrame(cl.frameUnreliable_, orkdemo::kMarkerUnreliable);

        CHECK(client->GetState() == ork::client::ClientState::Disconnected,
              "客户端初始状态 == Disconnected");

        client->Connect(&cl);

        // ---- 4. 手动驱动 + 双通道探针 ------------------------------------
        std::printf("\n[3] Reliable(KCP) + Unreliable(UDP) 回显\n");
        const double deadline = orkdemo::NowMs() + 20000.0;   // ctest TIMEOUT=120s，足够
        bool sentReliable = false;
        int  udpAttempts  = 0;
        double nextUdpAt  = 0.0;

        while (orkdemo::NowMs() < deadline)
        {
            client->PumpOnce();

            if (client->GetState() == ork::client::ClientState::Connected)
            {
                if (!sentReliable)
                {
                    const ork::client::SendResult r = client->Send(
                        cl.frameReliable_, TransportChannel::Reliable);
                    sentReliable = (r == ork::client::SendResult::Ok);
                    std::printf("      -> Reliable   %zu B send=%s\n",
                                cl.frameReliable_.size(),
                                std::string(ork::platform::ToString(r)).c_str());
                }
                // UDP 路由在握手刚完成时可能还没就绪：150ms 一次，最多 8 次
                if (!cl.unreliableEcho() && udpAttempts < 8 && orkdemo::NowMs() >= nextUdpAt)
                {
                    const ork::client::SendResult r = client->Send(
                        cl.frameUnreliable_, TransportChannel::Unreliable);
                    ++udpAttempts;
                    nextUdpAt = orkdemo::NowMs() + 150.0;
                    std::printf("      -> Unreliable %zu B attempt=%d send=%s\n",
                                cl.frameUnreliable_.size(), udpAttempts,
                                std::string(ork::platform::ToString(r)).c_str());
                }
                if (cl.reliableEcho() && cl.unreliableEcho()) break;
            }
            orkdemo::SleepMs(5);
        }

        CHECK(cl.connected(),      "客户端进入 Connected");
        CHECK(cl.reliableEcho(),   "★ Reliable(KCP) 25B 回显逐字节一致");
        CHECK(cl.unreliableEcho(), "★ Unreliable(UDP) 25B 回显逐字节一致");
        CHECK_EQ(cl.mismatches(),  0u, "客户端无字节不一致");
        CHECK_EQ(cl.payloads(),    2u, "客户端恰好收到 2 帧回显");
        CHECK(cl.connectionId() != 0u, "客户端拿到非零 connectionId");

        // ---- 5. 服务端侧证据 ---------------------------------------------
        std::printf("\n[4] 服务端侧证据（IConnectionRegistry + IHostListener）\n");
        // 给 host 的 pump 线程一点时间把统计与连接表刷新
        const double settle = orkdemo::NowMs() + 1500.0;
        while (orkdemo::NowMs() < settle) { client->PumpOnce(); orkdemo::SleepMs(20); }

        CHECK_EQ(listener->connections(), 1u, "host OnConnected 触发 1 次");
        CHECK_EQ(listener->payloads(),    2u, "host 收到 2 帧 payload");
        CHECK_EQ(listener->echoed(),      2u, "host 成功回显 2 帧");
        CHECK_EQ(listener->failures(),    0u, "host 回显无失败");
        CHECK_EQ(listener->cryptoFails(), 0u, "host crypto_fail_* 全 0");
        CHECK(listener->bytesIn()  > 0u, "host stats.bytesIn > 0");
        CHECK(listener->bytesOut() > 0u, "host stats.bytesOut > 0");
        CHECK_EQ(registry.Count(), 1u, "★ registry.Count() == 1");

        ConnectionView view{};
        const bool found = registry.TryGet(listener->lastConnectionId(), view);
        CHECK(found, "registry.TryGet(lastConnectionId) 命中");
        if (found)
        {
            CHECK(view.encryptionActive,
                  "★ ConnectionView.encryptionActive == true（AEAD 已协商）");
            CHECK(view.linkState == TransportLinkState::Established,
                  "★ ConnectionView.linkState == Established");
            CHECK(view.connectionId == listener->lastConnectionId(),
                  "ConnectionView.connectionId 与 OnConnected 一致");
            CHECK(view.lastActiveUs > 0u, "ConnectionView.lastActiveUs > 0");
        }
        // 快照路径也要给出同一条连接
        CHECK_EQ(registry.Snapshot().size(), 1u, "registry.Snapshot().size() == 1");

        // ---- 6. 干净收尾 -------------------------------------------------
        std::printf("\n[5] 收尾\n");
        client->Disconnect();
        for (int i = 0; i < 40 && !cl.disconnected(); ++i)
        {
            client->PumpOnce();
            orkdemo::SleepMs(5);
        }
        client.reset();
        clientAlive = false;
        io.reset();
        CHECK(cl.disconnected(), "客户端 OnDisconnected 已触发");

        // host 侧断开通知是异步的，等一下再查计数
        const double dwait = orkdemo::NowMs() + 1500.0;
        while (orkdemo::NowMs() < dwait && listener->disconnections() == 0)
            orkdemo::SleepMs(20);
        CHECK(listener->disconnections() >= 1u, "host OnDisconnected 已触发");

        framework->Stop();
        hostStarted = false;
        framework->RegisterListener(nullptr);
        framework.reset();
        listener.reset();
        CHECK(true, "Stop + RegisterListener(nullptr) + 析构未 hang / 未崩");
    }
    catch (const std::exception& e)
    {
        std::printf("  [FAIL] 未捕获异常: %s\n", e.what());
        ++g_failures;
        return finish(1);
    }
    catch (...)
    {
        std::printf("  [FAIL] 未捕获的未知异常\n");
        ++g_failures;
        return finish(1);
    }

    return finish(g_failures == 0 ? 0 : 1);
}
