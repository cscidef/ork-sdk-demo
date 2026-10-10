/**
 * @file main.cpp
 * @brief Demo 03 —— C++20 SDK 服务端（SessionFramework + IHostListener + ISender）
 *
 * 与 demo 01（纯 C ABI）的差别
 * ----------------------------
 * | 维度        | 01 C ABI Host                | 03 C++ SDK Host                  |
 * |-------------|------------------------------|----------------------------------|
 * | 入口        | ork_host_create/start/pump   | SessionFramework::Create/Start   |
 * | 回调        | C 函数指针 + void* user      | 继承 ork::sdk::IHostListener     |
 * | payload     | const uint8_t* + uint32_t    | std::span<const uint8_t>         |
 * | 发送        | ork_host_send / _broadcast   | ork::sdk::ISender::Send/Broadcast|
 * | 连接治理    | ork_host_get_connection_info | IConnectionRegistry（Snapshot 等）|
 * | bootstrap   | 恒 PlaintextDevelopment      | 可配 TLS 1.3（生产安全默认）      |
 *
 * C++ 接入面多出两件关键能力：
 *   1. `TransportConfig::bootstrapSecurity` 可以选 TlsServerAuthenticated，
 *      配上证书链 / 私钥就是生产可用的安全 bootstrap（C ABI 目前只有明文）；
 *   2. `IConnectionRegistry` 提供 Count / TryGet / ForEach / Snapshot /
 *      Disconnect 五个方法，宿主可以自己做会话表、踢人、限流。
 *
 * 线程契约（见 ork/sdk/host_listener.hpp）
 * ----------------------------------------
 *   - 四个回调都在 core 内部 NetworkPump 的 asio strand 线程上触发；
 *   - 实现必须非阻塞：不要在回调里做磁盘 IO / 加锁长等；
 *   - **禁止**在回调里调 SessionFramework::Stop() 或析构框架（会 join 自己
 *     所在线程 → 死锁）；
 *   - IConnectionRegistry::Disconnect() 与 ISender::Send() 在回调里调用是安全的；
 *   - OnPayload 的 span 只在本次回调内有效，要留存必须自己拷贝。
 *
 * 回显监听器（orkdemo::EchoListener，与 test_cpp_roundtrip 共用同一份实现）
 * 就是这个契约的最小示范：只做计数 + 入队，不碰 Stop / 不做阻塞 IO。
 *
 * 用法
 * ----
 *   ./03_cpp_host                        # 监听 9400
 *   ./03_cpp_host --port 9500
 *   ./03_cpp_host --port 9500 --no-encryption
 *   ORK_LOG_LEVEL=debug ORK_LOG_FILE=/tmp/ork.log ./03_cpp_host
 */

#include "demo_support.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

std::atomic<bool> g_stop{false};

void Usage(const char* prog)
{
    std::printf("用法: %s [--port P] [--no-encryption] [--help]\n"
                "  --port           TCP bootstrap 与 UDP/KCP 共用的监听端口（默认 9400）\n"
                "  --no-encryption  关闭 AEAD 加密（默认开启）\n"
                "\n环境变量:\n"
                "  ORK_LOG_FILE   内部日志文件（默认 /dev/null）\n"
                "  ORK_LOG_LEVEL  trace|debug|info|warn|error|off（默认 warn）\n",
                prog);
}

}  // namespace

int main(int argc, char** argv)
{
    uint16_t port = 9400;
    bool encryption = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
        if (a == "--no-encryption") { encryption = false; continue; }
        if (a == "--port" && i + 1 < argc)
        {
            const long p = std::strtol(argv[++i], nullptr, 10);
            if (p <= 0 || p > 65535) { std::fprintf(stderr, "[host] 非法端口\n"); return 2; }
            port = static_cast<uint16_t>(p);
            continue;
        }
        std::fprintf(stderr, "[host] 未知参数 %s\n", a.c_str());
        Usage(argv[0]);
        return 2;
    }

    orkdemo::InitLogging("03_cpp_host");

    orkdemo::PrintBannerRule();
    std::printf(" Demo 03 · C++ SDK host (SessionFramework + IHostListener)\n");
    std::printf(" port=%u  encryption=%s  bootstrap=PlaintextDevelopment\n",
                static_cast<unsigned>(port), encryption ? "on" : "off");
    orkdemo::PrintBannerRule();

    try
    {
        // ---- 1. 配置 + 创建框架 ----------------------------------------
        // Create() 会做完整校验（gamePort != 0、kcpUpdateRate >= pumpRate、
        // maxPacketSize ∈ [50,1414]、TLS 模式下证书存在且私钥 owner-only …），
        // 不合法直接抛 std::runtime_error —— fail closed，没有静默降级。
        const TransportConfig cfg = orkdemo::MakeHostConfig(port, encryption);
        std::unique_ptr<SessionFramework> framework = SessionFramework::Create(cfg);

        // ---- 2. 注册监听器（必须在 Start 之前）--------------------------
        // GetSender() 在 Create 之后就可用，因此 listener 可以直接持有引用。
        // verbose=true：demo 要逐帧打印回显（test_cpp_roundtrip 传 false）。
        orkdemo::EchoListener listener(framework->GetSender(), /*verbose=*/true);
        framework->RegisterListener(&listener);

        // ---- 3. 启动 ----------------------------------------------------
        framework->Start();

        orkdemo::InstallSignalHandlers(g_stop);
        std::printf("[host] listening on 127.0.0.1:%u  (TCP bootstrap + UDP/KCP)\n",
                    static_cast<unsigned>(port));
        std::printf("[host] maxPacketSize=%d B  pumpRate=%u Hz  kcpUpdateRate=%u Hz\n",
                    cfg.maxPacketSize, cfg.pumpRate, cfg.kcpUpdateRate);
        std::printf("[host] 试试: ./04_cpp_client --port %u\n", static_cast<unsigned>(port));
        std::printf("[host] 按 Ctrl+C 退出\n");
        std::fflush(stdout);

        // ---- 4. 主线程只做等待 + 心跳 ----------------------------------
        // 网络 pump、KCP update、宿主回调全部在 core 内部线程上跑。
        IConnectionRegistry& registry = framework->GetConnectionRegistry();
        const double t0 = orkdemo::NowMs();
        double lastReport = t0;

        while (!g_stop.load(std::memory_order_relaxed))
        {
            orkdemo::SleepMs(50);

            const double t = orkdemo::NowMs();
            if (t - lastReport >= 10000.0)
            {
                lastReport = t;

                // IConnectionRegistry 的三个只读能力
                const size_t viaCount = registry.Count();
                size_t viaSnapshot = 0;
                registry.ForEach([&](const ConnectionView&) { ++viaSnapshot; });

                std::printf("[host] up=%.1fs  active=%zu(count)/%zu(forEach)  "
                            "payload=%llu  echoed=%llu  fail=%llu  "
                            "in=%llu B/%llu pkt  out=%llu B  cryptoFail=%llu\n",
                            (t - t0) / 1000.0, viaCount, viaSnapshot,
                            static_cast<unsigned long long>(listener.payloads()),
                            static_cast<unsigned long long>(listener.echoed()),
                            static_cast<unsigned long long>(listener.failures()),
                            static_cast<unsigned long long>(listener.bytesIn()),
                            static_cast<unsigned long long>(listener.packetsIn()),
                            static_cast<unsigned long long>(listener.bytesOut()),
                            static_cast<unsigned long long>(listener.cryptoFails()));
                std::fflush(stdout);
            }
        }

        // ---- 5. 干净退出 ------------------------------------------------
        std::printf("\n[host] shutting down...\n");
        std::fflush(stdout);

        framework->Stop();          // 不得在网络 pump 线程内调用（这里在主线程，OK）
        framework->RegisterListener(nullptr);   // 先摘监听再析构，杜绝悬垂回调

        std::printf("[host] 累计: payload=%llu echoed=%llu fail=%llu cryptoFail=%llu\n",
                    static_cast<unsigned long long>(listener.payloads()),
                    static_cast<unsigned long long>(listener.echoed()),
                    static_cast<unsigned long long>(listener.failures()),
                    static_cast<unsigned long long>(listener.cryptoFails()));
        std::printf("[host] bye\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[host] Fatal: %s\n", e.what());
        return 1;
    }
}
