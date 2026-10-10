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
 *   7. finish() 统一收尾：Disconnect + Stop + RegisterListener(nullptr)
 *      不 hang、不崩 —— 成功路径与异常路径共用同一份清理代码。
 *
 * 监听器（EchoListener / EchoProbeListener）、双通道探针循环
 * （RunDualChannelProbe）与断言框架（ork_test_assert.h）都来自共享头，
 * 本文件只剩「跑一遍真实往返 + 收集证据」。
 *
 * 端口在运行时向 OS 申请（TCP+UDP 同时空闲），所以可以 `ctest -j8` 并行，
 * 也不会和 9400 上手工跑的 demo 抢端口。
 *
 * 退出码：0 = 全部断言通过；1 = 有断言失败或异常。
 */

#include "demo_support.hpp"

#include "ork_test_assert.h"        // CHECK / CHECK_EQ / ORK_BANNER_RULE

#include "ork/client/ork_client.hpp"
#include "ork/sdk/sdk/session_interface.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace {

// ============================================================================
// 双通道探针的 Driver 适配（F2）
//
// 循环节奏（先发 Reliable、Unreliable 按 150ms 重试至多 8 次、上一轮回显到手
// 才发下一轮）由 orkdemo::RunDualChannelProbe 统一给出，这里只回答
// 「怎么 pump、怎么发、怎么打印」。
// ============================================================================
struct RoundtripProbeDriver
{
    ork::client::OrkClient&     client;
    orkdemo::EchoProbeListener& probe;

    bool ShouldStop()   const { return false; }   // 测试不响应 Ctrl+C
    bool Pump()               { client.PumpOnce(); return true; }
    void OnTick()             { }                 // 不打状态迁移，保持 ctest 输出干净
    bool Connected()    const { return client.GetState() == ork::client::ClientState::Connected; }
    bool Disconnected() const { return false; }   // 靠超时兜底，与改造前一致
    bool ReliableEchoed()   const { return probe.reliableEcho(); }
    bool UnreliableEchoed() const { return probe.unreliableEcho(); }
    void ClearReliableEcho()      { probe.clearReliableEcho(); }

    // 两帧探针在 EchoProbeListener 构造时就已就位，这里**不重盖时间戳**：
    // Unreliable 会重试，重盖会让上一帧的迟到回显被误判成 mismatch。
    bool SendReliable(int /*round*/)
    {
        const ork::client::SendResult r =
            client.Send(probe.frameReliable(), TransportChannel::Reliable);
        std::printf("      -> Reliable   %zu B send=%s\n",
                    probe.frameReliable().size(),
                    std::string(ork::platform::ToString(r)).c_str());
        return r == ork::client::SendResult::Ok;
    }

    bool SendUnreliable(int attempt)
    {
        const ork::client::SendResult r =
            client.Send(probe.frameUnreliable(), TransportChannel::Unreliable);
        std::printf("      -> Unreliable %zu B attempt=%d send=%s\n",
                    probe.frameUnreliable().size(), attempt,
                    std::string(ork::platform::ToString(r)).c_str());
        return r == ork::client::SendResult::Ok;
    }
};

}  // namespace

// ============================================================================
int main()
{
    // 默认静音（内部日志级别 Off + 文件 /dev/null）。排障时：
    //   ORK_LOG_FILE=/tmp/ork.log ORK_LOG_LEVEL=debug ./test_cpp_roundtrip
    orkdemo::InitLogging("test_cpp_roundtrip", LogLevel::Off);

    orkdemo::PrintBanner("test_cpp_roundtrip · C++ SDK host + client in one process");

    // 失败也要把 host/client 收尾干净：清理只写一遍（下面的 finish），
    // 成功路径与异常路径都走它。
    std::unique_ptr<SessionFramework>       framework;
    std::unique_ptr<orkdemo::EchoListener>  listener;
    std::unique_ptr<asio::io_context>       io;
    std::unique_ptr<ork::client::OrkClient> client;
    bool clientAlive = false;
    bool hostStarted = false;

    // 声明在 try 之外：异常路径上 finish() 里的 Disconnect() 仍可能触发回调，
    // 监听器必须活到 client 析构之后。verbose=false → 只统计，不打业务日志。
    orkdemo::EchoProbeListener cl(/*verbose=*/false);

    auto finish = [&](int code) -> int {
        const bool didTeardown = (framework != nullptr) || clientAlive;

        if (clientAlive)
        {
            // 成功路径上主流程已经 Disconnect 过了，别重复调
            if (!cl.disconnected()) client->Disconnect();
            // 把 Disconnect 的异步收尾 pump 干净，避免析构时还有 in-flight 回调
            for (int i = 0; i < 40; ++i) { client->PumpOnce(); orkdemo::SleepMs(5); }
            client.reset();
            clientAlive = false;
        }
        io.reset();
        if (framework)
        {
            if (hostStarted) framework->Stop();
            framework->RegisterListener(nullptr);   // 先摘监听再析构，杜绝悬垂回调
            framework.reset();
        }
        listener.reset();

        // 走到这里说明整条拆除链没 hang、没崩。
        // 端口都没申请到就早退的路径上，这条断言没有意义，不刷。
        if (didTeardown)
            CHECK(true, "Stop + RegisterListener(nullptr) + 析构未 hang / 未崩");

        std::printf("\n" ORK_BANNER_RULE "\n");
        if (code == 0)
            std::printf("PASS: test_cpp_roundtrip  (%d/%d checks)\n",
                        g_checks - g_failures, g_checks);
        else
            std::printf("FAIL: test_cpp_roundtrip  (%d failures / %d checks)\n",
                        g_failures, g_checks);
        std::printf(ORK_BANNER_RULE "\n");
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

        listener = std::make_unique<orkdemo::EchoListener>(framework->GetSender(),
                                                           /*verbose=*/false);
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

        CHECK(client->GetState() == ork::client::ClientState::Disconnected,
              "客户端初始状态 == Disconnected");

        client->Connect(&cl);

        // ---- 4. 手动驱动 + 双通道探针 ------------------------------------
        std::printf("\n[3] Reliable(KCP) + Unreliable(UDP) 回显\n");
        RoundtripProbeDriver drv{*client, cl};
        orkdemo::DualChannelProbeParams params;
        params.timeoutMs = 20000;   // ctest TIMEOUT=120s，足够
        const orkdemo::DualChannelProbeResult probe =
            orkdemo::RunDualChannelProbe(drv, params);

        CHECK(!probe.timedOut,     "双通道探针在 20s 内收敛");
        CHECK(cl.connected(),      "客户端进入 Connected");
        CHECK(probe.reliableOk,    "★ Reliable(KCP) 25B 回显逐字节一致");
        CHECK(probe.unreliableOk,  "★ Unreliable(UDP) 25B 回显逐字节一致");
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
        // 只有「必须在 framework->Stop() 之前查」的两条断言留在这里，
        // 真正的拆除动作全部交给 finish()（F11：不再内联重写一遍）。
        std::printf("\n[5] 收尾\n");
        client->Disconnect();
        for (int i = 0; i < 40 && !cl.disconnected(); ++i)
        {
            client->PumpOnce();
            orkdemo::SleepMs(5);
        }
        CHECK(cl.disconnected(), "客户端 OnDisconnected 已触发");

        // host 侧断开通知是异步的，等一下再查计数
        const double dwait = orkdemo::NowMs() + 1500.0;
        while (orkdemo::NowMs() < dwait && listener->disconnections() == 0)
            orkdemo::SleepMs(20);
        CHECK(listener->disconnections() >= 1u, "host OnDisconnected 已触发");
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
