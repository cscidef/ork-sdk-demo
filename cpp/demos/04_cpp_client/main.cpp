/**
 * @file main.cpp
 * @brief Demo 04 —— C++20 SDK 客户端（OrkClient + IClientListener）
 *
 * 连接流程（OrkClient 内部完成，全部在驱动线程上异步推进）
 *   TCP connect → Bootstrap V2（109B opaque 帧：connectionId / sessionToken /
 *   c2sKey / s2cKey / bootstrapEpoch / 加密标志）→ 绑定 UDP + 建 KCP 会话
 *   （conv = connectionId）→ UDP Handshake V2（42B 认证帧，HKDF 派生
 *   handshakeKey + HMAC-SHA256 authTag）→ 收到服务端经 KCP 下发的
 *   "ORK-UDP-ACK:handshake" → ClientState::Connected
 *
 * 手动驱动 vs 内置线程
 * --------------------
 * OrkClient 提供两种驱动方式，**二选一**：
 *   - `PumpOnce()`：非阻塞跑一遍就绪回调，由调用方的主循环反复调用
 *     （游戏主循环 / Unity Update 的标准形态，本 demo 用的就是它）；
 *   - `Start()`：派生一个内置 io 线程，此时从别的线程调 Send / Disconnect
 *     会被自动 post 到 io 线程串行执行（Send 返回 Ok 只代表入队成功）。
 * 禁止在回调里析构 OrkClient 或调 Stop()（会 join 自己所在的线程 → 死锁）。
 *
 * 用法
 * ----
 *   ./04_cpp_client                                   # 连 127.0.0.1:9400
 *   ./04_cpp_client --port 9500 --count 3
 *   ./04_cpp_client --host 127.0.0.1 --timeout 15 --no-encryption
 *
 * 退出码：0 = 两通道回显一致；1 = 失败 / 超时；2 = 参数错误。
 */

#include "demo_support.hpp"

#include "ork/client/ork_client.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace {

std::atomic<bool> g_stop{false};

// ----------------------------------------------------------------------------
// DemoListener —— IClientListener 的三个回调都有默认空实现，按需覆盖
// ----------------------------------------------------------------------------
class DemoListener final : public ork::client::IClientListener
{
public:
    void OnConnected(uint32_t connectionId) override
    {
        connectionId_ = connectionId;
        connected_.store(true, std::memory_order_relaxed);
        std::printf("[client] OnConnected  connectionId=%u\n", connectionId);
        std::fflush(stdout);
    }

    void OnDisconnected(uint32_t reason) override
    {
        disconnectReason_ = reason;
        disconnected_.store(true, std::memory_order_relaxed);
        std::printf("[client] OnDisconnected reason=%u\n", reason);
        std::fflush(stdout);
    }

    void OnPayload(std::span<const uint8_t> data, TransportChannel channel) override
    {
        payloads_++;
        const bool reliable = (channel == TransportChannel::Reliable);
        const std::span<const uint8_t> expect =
            reliable ? std::span<const uint8_t>(frameReliable_)
                     : std::span<const uint8_t>(frameUnreliable_);

        if (data.size() != orkdemo::kEchoFrameSize || !orkdemo::IsEchoMagic(data))
        {
            std::printf("[client] OnPayload %s %zu B —— 非 ORKBENCH1 帧\n",
                        reliable ? "Reliable" : "Unreliable", data.size());
            std::fflush(stdout);
            return;
        }

        if (std::memcmp(data.data(), expect.data(), expect.size()) == 0)
        {
            if (reliable) reliableEcho_ = true; else unreliableEcho_ = true;

            const uint64_t sentNs =
                orkdemo::GetU64Le(data.data() + orkdemo::kEchoMagic.size() + 8);
            const long long rttUs =
                static_cast<long long>((orkdemo::NowNs() - sentNs) / 1000ull);
            std::printf("[client] OnPayload %s echo OK (%zu B, rtt~%lld us)\n",
                        reliable ? "Reliable  " : "Unreliable", data.size(), rttUs);
        }
        else
        {
            std::printf("[client] OnPayload %s echo MISMATCH\n",
                        reliable ? "Reliable" : "Unreliable");
        }
        std::fflush(stdout);
    }

    // ---- 主循环读写 ----
    bool connected()    const noexcept { return connected_.load(std::memory_order_relaxed); }
    bool disconnected() const noexcept { return disconnected_.load(std::memory_order_relaxed); }
    uint32_t connectionId() const noexcept { return connectionId_; }
    uint32_t disconnectReason() const noexcept { return disconnectReason_; }
    uint64_t payloads() const noexcept { return payloads_; }

    std::array<uint8_t, orkdemo::kEchoFrameSize> frameReliable_{};
    std::array<uint8_t, orkdemo::kEchoFrameSize> frameUnreliable_{};
    bool reliableEcho_   = false;
    bool unreliableEcho_ = false;

private:
    std::atomic<bool> connected_{false};
    std::atomic<bool> disconnected_{false};
    uint32_t connectionId_    = 0;
    uint32_t disconnectReason_ = 0;
    uint64_t payloads_        = 0;
};

void Usage(const char* prog)
{
    std::printf("用法: %s [--host H] [--port P] [--count N] [--timeout SEC]\n"
                "          [--no-encryption] [--help]\n"
                "  --host           服务端地址（默认 127.0.0.1；明文 bootstrap 只允许 loopback）\n"
                "  --port           TCP bootstrap 与 UDP/KCP 共用端口（默认 9400）\n"
                "  --count          Reliable 探针轮数（默认 1）\n"
                "  --timeout        整体超时秒数（默认 10）\n"
                "  --no-encryption  客户端加密意向置 false\n",
                prog);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    uint16_t port = 9400;
    int count = 1;
    int timeoutSec = 10;
    bool encryption = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
        if (a == "--no-encryption") { encryption = false; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "[client] %s 缺少参数值\n", a.c_str()); return 2; }
        const std::string v = argv[++i];
        if      (a == "--host")    host = v;
        else if (a == "--port")    port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (a == "--count")   count = std::atoi(v.c_str());
        else if (a == "--timeout") timeoutSec = std::atoi(v.c_str());
        else { std::fprintf(stderr, "[client] 未知参数 %s\n", a.c_str()); Usage(argv[0]); return 2; }
    }
    if (count < 1) count = 1;
    if (timeoutSec < 1) timeoutSec = 1;

    orkdemo::InitLogging("04_cpp_client");

    std::printf("=============================================================\n");
    std::printf(" Demo 04 · C++ SDK client (OrkClient + IClientListener)\n");
    std::printf(" target=%s:%u  encryption=%s  rounds=%d\n",
                host.c_str(), static_cast<unsigned>(port),
                encryption ? "on" : "off", count);
    std::printf("=============================================================\n");

    try
    {
        // io_context 必须比 OrkClient 活得久（OrkClient 持引用），
        // 因此声明顺序 = io 在前、client 在后，析构顺序自然相反。
        asio::io_context io;

        const ork::client::ClientConfig cfg =
            orkdemo::MakeClientConfig(host, port, encryption);

        ork::client::OrkClient client(io, cfg);
        DemoListener listener;

        orkdemo::InstallSignalHandlers(g_stop);

        std::printf("[client] state=%s\n", ork::client::ToString(client.GetState()));
        client.Connect(&listener);

        constexpr int kUdpMaxAttempts = 8;
        constexpr int kUdpRetryMs     = 150;

        const double t0 = orkdemo::NowMs();
        const double deadline = t0 + static_cast<double>(timeoutSec) * 1000.0;

        int reliableRounds = 0;
        bool firstReliableSent = false;
        int udpAttempts = 0;
        double nextUdpTry = 0.0;
        auto lastState = client.GetState();

        while (!g_stop.load(std::memory_order_relaxed))
        {
            // ★ 手动驱动：不调 PumpOnce，连接流程一步都不会推进。
            client.PumpOnce();

            const double t = orkdemo::NowMs();

            const auto st = client.GetState();
            if (st != lastState)
            {
                std::printf("[client] state: %s -> %s\n",
                            ork::client::ToString(lastState),
                            ork::client::ToString(st));
                std::fflush(stdout);
                lastState = st;
            }

            if (listener.connected())
            {
                // 第一轮 Reliable 探针：连接就绪后立刻发。
                // （此刻 KCP 会话已建好——OrkClient 先 StartKcpSession()
                //   再把状态置为 Connected 并回调 OnConnected。）
                if (!firstReliableSent)
                {
                    orkdemo::BuildEchoFrame(listener.frameReliable_,
                                            orkdemo::kMarkerReliable);
                    const auto r = client.Send(listener.frameReliable_,
                                               TransportChannel::Reliable);
                    if (r == ork::client::SendResult::Ok) { firstReliableSent = true; reliableRounds = 1; }
                    std::printf("[client] -> Reliable(KCP)   %zu B round=%d send=%s\n",
                                listener.frameReliable_.size(), reliableRounds,
                                std::string(ork::platform::ToString(r)).c_str());
                    std::fflush(stdout);
                }

                // Unreliable 走裸 UDP，回显可能丢；重发几次是正常做法。
                if (!listener.unreliableEcho_ && udpAttempts < kUdpMaxAttempts
                    && t >= nextUdpTry)
                {
                    orkdemo::BuildEchoFrame(listener.frameUnreliable_,
                                            orkdemo::kMarkerUnreliable);
                    const auto r = client.Send(listener.frameUnreliable_,
                                               TransportChannel::Unreliable);
                    udpAttempts++;
                    nextUdpTry = t + kUdpRetryMs;
                    std::printf("[client] -> Unreliable(UDP) %zu B attempt=%d send=%s\n",
                                listener.frameUnreliable_.size(), udpAttempts,
                                std::string(ork::platform::ToString(r)).c_str());
                    std::fflush(stdout);
                }

                // 后续 Reliable 轮次：上一轮回显到手才发下一轮（天然限流）。
                if (listener.reliableEcho_ && reliableRounds < count)
                {
                    orkdemo::BuildEchoFrame(listener.frameReliable_,
                                            orkdemo::kMarkerReliable);
                    const auto r = client.Send(listener.frameReliable_,
                                               TransportChannel::Reliable);
                    if (r == ork::client::SendResult::Ok) reliableRounds++;
                    listener.reliableEcho_ = false;   // 等这一轮的回显
                    std::printf("[client] -> Reliable(KCP)   %zu B round=%d send=%s\n",
                                listener.frameReliable_.size(), reliableRounds,
                                std::string(ork::platform::ToString(r)).c_str());
                    std::fflush(stdout);
                }

                // 收敛条件：最后一轮 Reliable 回显到手 + Unreliable 有结果
                if (reliableRounds >= count && listener.reliableEcho_
                    && (listener.unreliableEcho_ || udpAttempts >= kUdpMaxAttempts))
                    break;
            }

            if (listener.disconnected()) break;
            if (t >= deadline) { std::fprintf(stderr, "[client] 超时（%d s）\n", timeoutSec); break; }

            orkdemo::SleepMs(1);
        }

        const bool reliableOk = (reliableRounds >= count) && listener.reliableEcho_;
        const bool unreliableOk = listener.unreliableEcho_;

        if (!listener.disconnected())
        {
            client.Disconnect();
            const double discDeadline = orkdemo::NowMs() + 2000.0;
            while (!listener.disconnected()
                   && orkdemo::NowMs() < discDeadline
                   && !g_stop.load(std::memory_order_relaxed))
            {
                client.PumpOnce();
                orkdemo::SleepMs(1);
            }
        }

        std::printf("[client] final state=%s connectionId=%u payloads=%llu\n",
                    ork::client::ToString(client.GetState()),
                    client.GetConnectionId(),
                    static_cast<unsigned long long>(listener.payloads()));

        std::printf("-------------------------------------------------------------\n");
        std::printf(" Reliable(KCP)   echo : %s (%d/%d rounds)\n",
                    reliableOk ? "OK" : "FAIL", reliableRounds, count);
        std::printf(" Unreliable(UDP) echo : %s (%d attempts)\n",
                    unreliableOk ? "OK" : "FAIL", udpAttempts);
        std::printf("-------------------------------------------------------------\n");

        const bool pass = listener.connected() && reliableOk && unreliableOk
                          && !g_stop.load(std::memory_order_relaxed);
        std::printf("%s\n", pass ? "PASS: C++ SDK echo roundtrip"
                                 : "FAIL: C++ SDK echo roundtrip");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[client] Fatal: %s\n", e.what());
        return 1;
    }
}
