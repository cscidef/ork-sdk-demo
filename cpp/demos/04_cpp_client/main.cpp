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
 * 本文件只回答「怎么 pump、怎么发、怎么打印」；双通道探针的节奏（先发
 * Reliable、Unreliable 按 150ms 重试至多 8 次、上一轮回显到手才发下一轮）
 * 由 demo_support.hpp 的 `RunDualChannelProbe` 给出，回显比对与 RTT 统计
 * 由共享的 `EchoProbeListener` 给出 —— 与 test_cpp_roundtrip 同一份实现。
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::atomic<bool> g_stop{false};

// ----------------------------------------------------------------------------
// 双通道探针的 Driver 适配（F2）
// ----------------------------------------------------------------------------
struct ClientProbeDriver
{
    ork::client::OrkClient&     client;
    orkdemo::EchoProbeListener& listener;
    ork::client::ClientState    lastState;

    bool ShouldStop() const noexcept { return g_stop.load(std::memory_order_relaxed); }

    // ★ 手动驱动：不调 PumpOnce，连接流程一步都不会推进。
    // PumpOnce() 返回 void，出错走异常，所以恒 true。
    bool Pump() { client.PumpOnce(); return true; }

    void OnTick()
    {
        const ork::client::ClientState st = client.GetState();
        if (st == lastState) return;
        std::printf("[client] state: %s -> %s\n",
                    ork::client::ToString(lastState), ork::client::ToString(st));
        std::fflush(stdout);
        lastState = st;
    }

    bool Connected()    const noexcept { return listener.connected(); }
    bool Disconnected() const noexcept { return listener.disconnected(); }
    bool ReliableEchoed()   const noexcept { return listener.reliableEcho(); }
    bool UnreliableEchoed() const noexcept { return listener.unreliableEcho(); }
    void ClearReliableEcho() noexcept { listener.clearReliableEcho(); }

    // 每次发送前重盖时间戳：本 demo 的教学点之一就是 RTT，量到的必须是这一轮。
    bool SendReliable(int round)
    {
        listener.RebuildReliableFrame();
        const auto r = client.Send(listener.frameReliable(), TransportChannel::Reliable);
        std::printf("[client] -> Reliable(KCP)   %zu B round=%d send=%s\n",
                    listener.frameReliable().size(), round,
                    std::string(ork::platform::ToString(r)).c_str());
        std::fflush(stdout);
        return r == ork::client::SendResult::Ok;
    }

    // Unreliable 走裸 UDP，回显可能丢；重发几次是正常做法。
    bool SendUnreliable(int attempt)
    {
        listener.RebuildUnreliableFrame();
        const auto r = client.Send(listener.frameUnreliable(), TransportChannel::Unreliable);
        std::printf("[client] -> Unreliable(UDP) %zu B attempt=%d send=%s\n",
                    listener.frameUnreliable().size(), attempt,
                    std::string(ork::platform::ToString(r)).c_str());
        std::fflush(stdout);
        return r == ork::client::SendResult::Ok;
    }
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

/**
 * @brief strtol + 严格范围校验（F19：不用 atoi —— 它把 "abc" 静默当成 0）
 * @param[out] rc  0 = 解析成功；2 = 非法（调用方应打印 Usage 后 return 2）
 */
int ParseIntArg(const char* what, const char* v, int lo, int hi, int* rc)
{
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < static_cast<long>(lo) || n > static_cast<long>(hi))
    {
        std::fprintf(stderr, "[client] %s 非法: \"%s\"（允许 %d..%d）\n", what, v, lo, hi);
        *rc = 2;
        return lo;
    }
    *rc = 0;
    return static_cast<int>(n);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    int port       = 9400;
    int count      = 1;
    int timeoutSec = 10;
    bool encryption = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
        if (a == "--no-encryption") { encryption = false; continue; }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "[client] %s 缺少参数值\n", a.c_str());
            Usage(argv[0]);
            return 2;
        }
        const char* v = argv[++i];
        int rc = 0;
        if      (a == "--host")    host = v;
        else if (a == "--port")    port = ParseIntArg("--port", v, 1, 65535, &rc);
        else if (a == "--count")   count = ParseIntArg("--count", v, 1, 1000000, &rc);
        else if (a == "--timeout") timeoutSec = ParseIntArg("--timeout", v, 1, 3600, &rc);
        else
        {
            std::fprintf(stderr, "[client] 未知参数 %s\n", a.c_str());
            Usage(argv[0]);
            return 2;
        }
        if (rc != 0) { Usage(argv[0]); return rc; }
    }

    orkdemo::InitLogging("04_cpp_client");

    orkdemo::PrintBannerRule();
    std::printf(" Demo 04 · C++ SDK client (OrkClient + IClientListener)\n");
    std::printf(" target=%s:%d  encryption=%s  rounds=%d\n",
                host.c_str(), port, encryption ? "on" : "off", count);
    orkdemo::PrintBannerRule();

    try
    {
        // io_context 必须比 OrkClient 活得久（OrkClient 持引用），
        // 因此声明顺序 = io 在前、client 在后，析构顺序自然相反。
        asio::io_context io;

        const ork::client::ClientConfig cfg = orkdemo::MakeClientConfig(
            host, static_cast<uint16_t>(port), encryption);

        ork::client::OrkClient client(io, cfg);
        // verbose=true：demo 要逐帧打印回显与 RTT
        orkdemo::EchoProbeListener listener(/*verbose=*/true);

        orkdemo::InstallSignalHandlers(g_stop);

        std::printf("[client] state=%s\n", ork::client::ToString(client.GetState()));
        client.Connect(&listener);

        ClientProbeDriver drv{client, listener, client.GetState()};
        orkdemo::DualChannelProbeParams params;
        params.reliableRounds = count;
        params.timeoutMs      = timeoutSec * 1000;
        params.pumpSleepMs    = 1;   // Client 靠调用方驱动，1ms 一拍足够灵敏

        const orkdemo::DualChannelProbeResult probe =
            orkdemo::RunDualChannelProbe(drv, params);
        if (probe.timedOut)
            std::fprintf(stderr, "[client] 超时（%d s）\n", timeoutSec);

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

        orkdemo::PrintSeparator();
        std::printf(" Reliable(KCP)   echo : %s (%d/%d rounds)\n",
                    probe.reliableOk ? "OK" : "FAIL", probe.reliableRounds, count);
        std::printf(" Unreliable(UDP) echo : %s (%d attempts)\n",
                    probe.unreliableOk ? "OK" : "FAIL", probe.udpAttempts);
        orkdemo::PrintSeparator();

        // 看 g_stop 而不是 probe.stopped：Ctrl+C 也可能落在探针之后的收尾阶段
        const bool pass = listener.connected() && probe.reliableOk && probe.unreliableOk
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
