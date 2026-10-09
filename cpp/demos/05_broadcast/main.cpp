/**
 * @file main.cpp
 * @brief Demo 05 —— C++20 广播：ISender::Broadcast + ConnectionSet + BroadcastReport
 *
 * 展示内容
 * --------
 * 服务端每收到一帧探针，就用 **三种目标集合表达** 各广播一次，并把
 * `BroadcastReport` 逐字段打出来：
 *
 * | 形态 | 表达式 | 语义 | marker | 每个客户端应收 |
 * |------|--------|------|--------|----------------|
 * | All   | `ConnectionSet::All()`                       | 所有活跃连接（含发起者） | `ORKBCSTA` | N 帧 |
 * | Where | `ConnectionSet::Where(id != origin)`         | 排除发起者（"转发给其他人"）| `ORKBCSTW` | N-1 帧 |
 * | Of    | `ConnectionSet::Of(registry.Snapshot() 的 id)` | 显式 ID 列表 | `ORKBCSTO` | N 帧 |
 *
 * 其中 N = 活跃连接数。三种帧用不同 marker 区分，客户端按 marker 分桶计数，
 * 于是"扇出是否正确"变成一组可以直接断言的整数。
 *
 * BroadcastReport 语义（见 sender.hpp / sender.cpp）
 * -------------------------------------------------
 *   attempted          本次遍历到的目标连接数
 *   sent               TrySend 成功数
 *   failedNotConnected All(): 连接已 Disconnected；Of(): TrySend 返回 false
 *   failedTooLarge     payload 超过 maxPacketSize（入口处只记一次，attempted=1）
 *   failedQueueFull    底层 TrySend 返回 false 的保守映射（All/Where 模式）
 *   failedCipher       预留；当前底层 bool 无法区分，恒 0
 *   IsConsistent()     attempted == 五个分类之和
 *
 * ★ 注意 All() 与 Where() 的 attempted 口径不同：All() 遍历传输层**全部**连接
 *   （含 Disconnected，每条都计入 attempted），Where() 直接跳过 Disconnected
 *   （不计入 attempted）。因此断言只对 `sent` 与 `IsConsistent()` 做，
 *   `attempted` 仅作展示 —— 这样刚断开一条连接的竞态不会让测试假失败。
 *
 * 两种运行模式
 * ------------
 *   --clients 0   纯服务端模式：起一个广播服务器，Ctrl+C 退出；
 *                 用多个终端跑 `./04_cpp_client --port P` 手工观察扇出。
 *   --clients N   自证模式（默认 N=3）：在**同一进程**内再起 N 个 OrkClient，
 *                 全部连上后齐射一帧，然后校验上面的 N / N-1 / N 矩阵，
 *                 打印 PASS/FAIL 并以 0/1 退出。
 *
 * 线程契约
 * --------
 * OnPayload 在 core 内部 pump 线程上触发。`ISender::Send/Broadcast` 与
 * `IConnectionRegistry::Snapshot/Disconnect` 都可以在回调里调用（只入队 /
 * 只读表 / 异步 close）；但 `SessionFramework::Stop()` 绝对不行 —— 它要 join
 * 当前这条线程，会自死锁。本 demo 的 Stop() 全部在主线程执行。
 *
 * 用法
 * ----
 *   ./05_broadcast                              # 端口 9400，自证模式 3 个客户端
 *   ./05_broadcast --clients 5 --port 9450
 *   ./05_broadcast --clients 0 --port 9400      # 纯服务端，等外部客户端
 *   ./05_broadcast --clients 2 --no-encryption
 *
 * 退出码：0 = 扇出矩阵与 BroadcastReport 全部符合预期；1 = 失败 / 超时；2 = 参数错误。
 */

#include "demo_support.hpp"

#include "ork/client/ork_client.hpp"
#include "ork/sdk/connection_set.hpp"
#include "ork/sdk/sender.hpp"
#include "ork/sdk/sdk/session_interface.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using ork::sdk::BroadcastReport;
using ork::sdk::Channel;
using ork::sdk::ConnectionSet;

// 四种 25B 探针帧的 marker（BuildEchoFrame 要求恰好 8 字节）
constexpr std::string_view kMarkerProbe = "ORKDEMO1";  // 客户端 -> 服务端
constexpr std::string_view kMarkerAll   = "ORKBCSTA";  // Broadcast(All())
constexpr std::string_view kMarkerWhere = "ORKBCSTW";  // Broadcast(Where(!= origin))
constexpr std::string_view kMarkerOf    = "ORKBCSTO";  // Broadcast(Of(ids))

std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
const char* ChannelName(Channel ch) noexcept
{
    return ch == Channel::Reliable ? "Reliable" : "Unreliable";
}

/** @brief 取 25B 帧的 marker（8 字节，不保证 NUL 结尾） */
std::string_view FrameMarker(std::span<const uint8_t> payload) noexcept
{
    if (payload.size() < orkdemo::kEchoMagic.size() + 8) return {};
    return {reinterpret_cast<const char*>(payload.data()) + orkdemo::kEchoMagic.size(), 8};
}

bool IsProbeFrame(std::span<const uint8_t> payload) noexcept
{
    return payload.size() == orkdemo::kEchoFrameSize
        && orkdemo::IsEchoMagic(payload)
        && FrameMarker(payload) == kMarkerProbe;
}

void PrintReport(const char* tag, const BroadcastReport& r)
{
    std::printf("[host]     %-7s attempted=%u sent=%u notConn=%u tooLarge=%u "
                "queueFull=%u cipher=%u  %s\n",
                tag, r.attempted, r.sent, r.failedNotConnected, r.failedTooLarge,
                r.failedQueueFull, r.failedCipher,
                r.IsConsistent() ? "consistent" : "!! INCONSISTENT !!");
    std::fflush(stdout);
}

// ===========================================================================
// 服务端监听器：把每一帧探针放大成三次广播
// ===========================================================================
class BroadcastListener final : public ork::sdk::IHostListener
{
public:
    BroadcastListener(ork::sdk::ISender& sender, IConnectionRegistry& registry) noexcept
        : sender_(sender), registry_(registry)
    {
    }

    void OnConnected(ConnectionId connectionId) override
    {
        const unsigned long long total =
            static_cast<unsigned long long>(connected_.fetch_add(1) + 1);
        std::printf("[host] + connection %u  (累计接入 %llu, 注册表活跃 %zu)\n",
                    connectionId, total, registry_.Count());
        std::fflush(stdout);
    }

    void OnDisconnected(ConnectionId connectionId, DisconnectReason reason) override
    {
        disconnected_.fetch_add(1);
        std::printf("[host] - connection %u  reason=%s  (注册表活跃 %zu)\n",
                    connectionId, ToString(reason), registry_.Count());
        std::fflush(stdout);
    }

    void OnPayload(ConnectionId origin, Channel channel,
                   std::span<const uint8_t> payload) override
    {
        payloads_.fetch_add(1);

        // 防环：只放大客户端探针帧。广播帧本身永远不会回到服务端，
        // 但若将来客户端把收到的帧再发回来，这里能挡住无限放大。
        if (!IsProbeFrame(payload))
            return;

        const unsigned round = rounds_.fetch_add(1) + 1;
        std::printf("[host] round %u: probe %zu B from conn %u via %s -> Broadcast x3\n",
                    round, payload.size(), origin, ChannelName(channel));

        std::array<uint8_t, orkdemo::kEchoFrameSize> frame{};

        // ---- 形态 1：All() —— 所有活跃连接（含发起者自己）----------------
        // 房间广播 / 全服公告的标准写法。payload 只拷贝一份，
        // 逐连接的独立加密由传输层完成。
        orkdemo::BuildEchoFrame(frame, kMarkerAll);
        const BroadcastReport rAll =
            sender_.Broadcast(ConnectionSet::All(), frame, Channel::Reliable);

        // ---- 形态 2：Where(predicate) —— 排除发起者 ----------------------
        // "转发给其他人"。谓词被 std::function 持有，Broadcast 同步返回，
        // 因此捕获栈上的 origin 是安全的。
        orkdemo::BuildEchoFrame(frame, kMarkerWhere);
        const BroadcastReport rWhere = sender_.Broadcast(
            ConnectionSet::Where([origin](ConnectionId id) { return id != origin; }),
            frame, Channel::Reliable);

        // ---- 形态 3：Of(ids) —— 显式 ID 列表 -----------------------------
        // ID 列表可以来自任何地方（队伍表、房间表、旁观者名单…）；
        // 这里从 IConnectionRegistry::Snapshot() 取全量，效果等同 All()，
        // 但走的是另一条代码路径（attempted 口径 = ids.size()）。
        std::vector<ConnectionId> ids;
        ids.reserve(registry_.Count());
        for (const ConnectionView& view : registry_.Snapshot())
            ids.push_back(view.connectionId);
        orkdemo::BuildEchoFrame(frame, kMarkerOf);
        const BroadcastReport rOf =
            sender_.Broadcast(ConnectionSet::Of(ids), frame, Channel::Reliable);

        if (rAll.IsConsistent() && rWhere.IsConsistent() && rOf.IsConsistent())
            consistentReports_.fetch_add(3);
        else
            inconsistentReports_.fetch_add(1);

        sentAll_.fetch_add(rAll.sent);
        sentWhere_.fetch_add(rWhere.sent);
        sentOf_.fetch_add(rOf.sent);

        PrintReport("All()",   rAll);
        PrintReport("Where()", rWhere);
        PrintReport("Of()",    rOf);
    }

    void OnStats(const ork::sdk::OrkStats& stats) override
    {
        // 只取标量，避免整个结构体跨线程拷贝时读到撕裂值
        active_.store(stats.activeConnections);
        bytesOut_.store(stats.bytesOut);
        dropsQueueFull_.store(stats.dropsQueueFull);
        dropsTooLarge_.store(stats.dropsTooLarge);
        cryptoFails_.store(stats.cryptoFailAead + stats.cryptoFailReplay
                           + stats.cryptoFailExpired + stats.cryptoFailMalformed);
    }

    // ---- 只读访问器（主线程用）----
    unsigned rounds()             const noexcept { return rounds_.load(); }
    unsigned payloads()           const noexcept { return payloads_.load(); }
    unsigned connected()          const noexcept { return connected_.load(); }
    unsigned disconnected()       const noexcept { return disconnected_.load(); }
    unsigned inconsistent()       const noexcept { return inconsistentReports_.load(); }
    unsigned long long sentAll()   const noexcept { return sentAll_.load(); }
    unsigned long long sentWhere() const noexcept { return sentWhere_.load(); }
    unsigned long long sentOf()    const noexcept { return sentOf_.load(); }
    unsigned long long cryptoFails() const noexcept { return cryptoFails_.load(); }
    unsigned long long drops()     const noexcept
    {
        return dropsQueueFull_.load() + dropsTooLarge_.load();
    }

private:
    ork::sdk::ISender& sender_;
    IConnectionRegistry& registry_;

    std::atomic<unsigned> rounds_{0};
    std::atomic<unsigned> payloads_{0};
    std::atomic<unsigned> connected_{0};
    std::atomic<unsigned> disconnected_{0};
    std::atomic<unsigned> consistentReports_{0};
    std::atomic<unsigned> inconsistentReports_{0};
    std::atomic<unsigned long long> sentAll_{0};
    std::atomic<unsigned long long> sentWhere_{0};
    std::atomic<unsigned long long> sentOf_{0};

    std::atomic<uint64_t> active_{0};
    std::atomic<uint64_t> bytesOut_{0};
    std::atomic<uint64_t> dropsQueueFull_{0};
    std::atomic<uint64_t> dropsTooLarge_{0};
    std::atomic<uint64_t> cryptoFails_{0};
};

// ===========================================================================
// 进程内客户端（--clients N 自证模式）
// ===========================================================================
class FanoutListener final : public ork::client::IClientListener
{
public:
    void OnConnected(uint32_t connectionId) override
    {
        connectionId_.store(connectionId);
        connected_.store(true);
    }

    void OnDisconnected(uint32_t reason) override
    {
        reason_.store(reason);
        disconnected_.store(true);
    }

    void OnPayload(std::span<const uint8_t> payload, TransportChannel channel) override
    {
        (void)channel;
        if (payload.size() != orkdemo::kEchoFrameSize || !orkdemo::IsEchoMagic(payload))
            return;

        const std::string_view marker = FrameMarker(payload);
        if      (marker == kMarkerAll)   all_.fetch_add(1);
        else if (marker == kMarkerWhere) where_.fetch_add(1);
        else if (marker == kMarkerOf)    of_.fetch_add(1);
        else                             other_.fetch_add(1);
    }

    bool     connected()    const noexcept { return connected_.load(); }
    bool     disconnected() const noexcept { return disconnected_.load(); }
    uint32_t connectionId() const noexcept { return connectionId_.load(); }
    uint32_t reason()       const noexcept { return reason_.load(); }
    unsigned all()          const noexcept { return all_.load(); }
    unsigned where()        const noexcept { return where_.load(); }
    unsigned of()           const noexcept { return of_.load(); }
    unsigned other()        const noexcept { return other_.load(); }

private:
    std::atomic<bool>     connected_{false};
    std::atomic<bool>     disconnected_{false};
    std::atomic<uint32_t> connectionId_{0};
    std::atomic<uint32_t> reason_{0};
    std::atomic<unsigned> all_{0};
    std::atomic<unsigned> where_{0};
    std::atomic<unsigned> of_{0};
    std::atomic<unsigned> other_{0};
};

/**
 * @brief 一个自带 io_context 的 OrkClient
 *
 * 成员声明顺序 **就是** 初始化顺序：io 必须比 client 先构造
 * （OrkClient 持 io_context& 引用），cfg 必须比 client 先就位。
 * OrkClient 内部按值保存 ClientConfig，因此 cfg 只需活过构造期，
 * 但为清晰起见仍让它与 client 同生命周期。
 */
struct FanoutClient
{
    explicit FanoutClient(int idx, const std::string& host, uint16_t port, bool encryption)
        : index(idx)
        , listener()
        , cfg(orkdemo::MakeClientConfig(host, port, encryption))
        , client(std::make_unique<ork::client::OrkClient>(io, cfg))
    {
    }

    const int index;
    asio::io_context io;
    FanoutListener listener;
    ork::client::ClientConfig cfg;
    std::unique_ptr<ork::client::OrkClient> client;
};

// ===========================================================================
// 进程内客户端容器
//
// RunFanoutSelfTest 里的 pumpAll lambda 与主流程都要访问它，用文件级容器最省事。
// 声明必须在使用之前（C++ 无前置引用）。
// ===========================================================================
std::vector<std::unique_ptr<FanoutClient>> clients_;

/** @brief 保证任何返回路径上客户端都被先断开再析构 */
struct ClientGuard
{
    ~ClientGuard()
    {
        for (const auto& c : clients_)
        {
            c->client->Disconnect();
            for (int i = 0; i < 20; ++i) { c->client->PumpOnce(); orkdemo::SleepMs(5); }
        }
        clients_.clear();
    }
};

// ===========================================================================
// 自证流程
// ===========================================================================
int RunFanoutSelfTest(SessionFramework& framework,
                      BroadcastListener& host,
                      int n,
                      uint16_t port,
                      bool encryption,
                      int timeoutSec)
{
    IConnectionRegistry& registry = framework.GetConnectionRegistry();
    const double deadline = orkdemo::NowMs() + static_cast<double>(timeoutSec) * 1000.0;
    ClientGuard guard;   // 任何 return 路径都会断开并释放客户端

    auto pumpAll = [&]() noexcept {
        for (const auto& c : clients_) c->client->PumpOnce();
    };

    // ---- 1. 起 N 个客户端并 Connect ------------------------------------
    std::printf("\n[test] 启动 %d 个进程内 OrkClient ...\n", n);
    for (int i = 0; i < n; ++i)
        clients_.push_back(std::make_unique<FanoutClient>(i, "127.0.0.1", port, encryption));
    for (const auto& c : clients_)
        c->client->Connect(&c->listener);

    // ---- 2. 等所有客户端进入 Connected ---------------------------------
    int up = 0;
    while (orkdemo::NowMs() < deadline)
    {
        pumpAll();
        up = 0;
        for (const auto& c : clients_) if (c->listener.connected()) ++up;
        if (up == n) break;
        orkdemo::SleepMs(5);
    }
    if (up != n)
    {
        std::fprintf(stderr, "[test] FAIL: 只有 %d/%d 个客户端连上（超时）\n", up, n);
        return 1;
    }

    // ---- 3. 等服务端注册表也看到 N 条活跃连接 ---------------------------
    // 否则先连上的客户端齐射时，广播扇出会小于 N，矩阵就不成立。
    while (orkdemo::NowMs() < deadline
           && registry.Count() < static_cast<size_t>(n))
    {
        pumpAll();
        orkdemo::SleepMs(5);
    }
    std::printf("[test] %d 个客户端已连接；服务端注册表活跃 = %zu\n",
                up, registry.Count());
    for (const auto& c : clients_)
    {
        ConnectionView view;
        const bool found = registry.TryGet(c->listener.connectionId(), view);
        std::printf("[test]   client#%d connId=%u  registry=%s link=%d encryption=%s\n",
                    c->index, c->listener.connectionId(),
                    found ? "found" : "MISSING",
                    found ? static_cast<int>(view.linkState) : -1,
                    (found && view.encryptionActive) ? "on" : "off");
    }
    std::fflush(stdout);

    if (registry.Count() < static_cast<size_t>(n))
    {
        std::fprintf(stderr, "[test] FAIL: 服务端只看到 %zu/%d 条活跃连接\n",
                     registry.Count(), n);
        return 1;
    }

    // ---- 4. 齐射：每个客户端各发一帧探针 --------------------------------
    std::printf("\n[test] 齐射 %d 帧探针 (Reliable, 25 B, marker=%.*s)\n",
                n, static_cast<int>(kMarkerProbe.size()), kMarkerProbe.data());
    std::array<uint8_t, orkdemo::kEchoFrameSize> probe{};
    int sendFailures = 0;
    for (const auto& c : clients_)
    {
        orkdemo::BuildEchoFrame(probe, kMarkerProbe);
        const ork::platform::SendResult r =
            c->client->Send(probe, TransportChannel::Reliable);
        if (r != ork::platform::SendResult::Ok)
        {
            ++sendFailures;
            std::fprintf(stderr, "[test] client#%d Send 失败: %s\n", c->index,
                         std::string(ork::platform::ToString(r)).c_str());
        }
    }

    // ---- 5. 等每个客户端收齐三类广播帧 ----------------------------------
    //   All()   -> n 帧（含自己那一轮）
    //   Where() -> n-1 帧（自己那一轮被谓词排除）
    //   Of()    -> n 帧
    const unsigned expectAll   = static_cast<unsigned>(n);
    const unsigned expectWhere = static_cast<unsigned>(n - 1);
    const unsigned expectOf    = static_cast<unsigned>(n);

    bool settled = false;
    while (orkdemo::NowMs() < deadline)
    {
        pumpAll();
        settled = true;
        for (const auto& c : clients_)
        {
            if (c->listener.all()   < expectAll   ||
                c->listener.where() < expectWhere ||
                c->listener.of()    < expectOf)
            {
                settled = false;
                break;
            }
        }
        if (settled) break;
        orkdemo::SleepMs(5);
    }

    // ---- 6. 打印客户端侧收帧矩阵 ----------------------------------------
    std::printf("\n[test] 客户端收帧矩阵（期望 All=%u Where=%u Of=%u）\n",
                expectAll, expectWhere, expectOf);
    std::printf("        %-8s %-9s %-9s %-9s %-7s\n",
                "client", "All()", "Where()", "Of()", "其他");
    bool clientMatrixOk = true;
    for (const auto& c : clients_)
    {
        const bool rowOk = c->listener.all()   == expectAll
                        && c->listener.where() == expectWhere
                        && c->listener.of()    == expectOf
                        && c->listener.other() == 0;
        clientMatrixOk = clientMatrixOk && rowOk;
        std::printf("        #%-7d %-9u %-9u %-9u %-7u %s\n",
                    c->index, c->listener.all(), c->listener.where(),
                    c->listener.of(), c->listener.other(), rowOk ? "OK" : "MISMATCH");
    }

    // ---- 7. 服务端侧汇总 -------------------------------------------------
    std::printf("\n[test] 服务端 BroadcastReport 汇总: rounds=%u sent(All)=%llu "
                "sent(Where)=%llu sent(Of)=%llu inconsistent=%u cryptoFail=%llu drops=%llu\n",
                host.rounds(), host.sentAll(), host.sentWhere(), host.sentOf(),
                host.inconsistent(), host.cryptoFails(), host.drops());
    std::fflush(stdout);

    const bool serverCountersOk =
        host.rounds()   == static_cast<unsigned>(n) &&
        host.sentAll()   == static_cast<unsigned long long>(n) * n &&
        host.sentWhere() == static_cast<unsigned long long>(n) * (n - 1) &&
        host.sentOf()    == static_cast<unsigned long long>(n) * n &&
        host.inconsistent() == 0 &&
        host.cryptoFails()  == 0 &&
        sendFailures == 0;

    // ---- 8. 断开全部客户端 ----------------------------------------------
    for (const auto& c : clients_) c->client->Disconnect();
    const double drainUntil = orkdemo::NowMs() + 2000.0;
    while (orkdemo::NowMs() < drainUntil)
    {
        pumpAll();
        orkdemo::SleepMs(10);
    }

    // ---- 9. 判定 ---------------------------------------------------------
    const bool ok = settled && clientMatrixOk && serverCountersOk;
    std::printf("\n-------------------------------------------------------------\n");
    std::printf(" clients=%d  settled=%s  clientMatrix=%s  serverCounters=%s\n",
                n, settled ? "yes" : "NO(timeout)",
                clientMatrixOk ? "OK" : "FAIL",
                serverCountersOk ? "OK" : "FAIL");
    std::printf("-------------------------------------------------------------\n");
    std::printf("%s\n", ok ? "PASS: ISender::Broadcast fanout (All / Where / Of)"
                           : "FAIL: broadcast fanout mismatch");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

void Usage(const char* prog)
{
    std::printf("用法: %s [--port P] [--clients N] [--timeout SEC] [--no-encryption] [--help]\n"
                "  --port           TCP bootstrap 与 UDP/KCP 共用的监听端口（默认 9400）\n"
                "  --clients N      0 = 纯服务端模式（等外部客户端，Ctrl+C 退出）\n"
                "                   N>0 = 进程内自证模式，起 N 个 OrkClient（默认 3）\n"
                "  --timeout SEC    自证模式总超时（默认 25）\n"
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
    int clientCount = 3;
    int timeoutSec = 25;
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
        if (a == "--clients" && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v < 0 || v > 64) { std::fprintf(stderr, "[host] --clients 需在 [0,64]\n"); return 2; }
            clientCount = static_cast<int>(v);
            continue;
        }
        if (a == "--timeout" && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v < 1 || v > 600) { std::fprintf(stderr, "[host] --timeout 需在 [1,600]\n"); return 2; }
            timeoutSec = static_cast<int>(v);
            continue;
        }
        std::fprintf(stderr, "[host] 未知参数 %s\n", a.c_str());
        Usage(argv[0]);
        return 2;
    }

    orkdemo::InitLogging("05_broadcast");

    std::printf("=============================================================\n");
    std::printf(" Demo 05 · ISender::Broadcast + ConnectionSet + BroadcastReport\n");
    std::printf(" port=%u  encryption=%s  clients=%d  timeout=%ds\n",
                static_cast<unsigned>(port), encryption ? "on" : "off",
                clientCount, timeoutSec);
    std::printf("=============================================================\n");

    try
    {
        const TransportConfig cfg = orkdemo::MakeHostConfig(port, encryption);
        std::unique_ptr<SessionFramework> framework = SessionFramework::Create(cfg);

        BroadcastListener listener(framework->GetSender(),
                                   framework->GetConnectionRegistry());
        framework->RegisterListener(&listener);
        framework->Start();

        orkdemo::InstallSignalHandlers(g_stop);
        std::printf("[host] listening on 127.0.0.1:%u  (TCP bootstrap + UDP/KCP)\n",
                    static_cast<unsigned>(port));
        std::fflush(stdout);

        int rc = 0;
        if (clientCount == 0)
        {
            std::printf("[host] 纯服务端模式。多开几个终端跑：\n");
            std::printf("         ./04_cpp_client --port %u\n", static_cast<unsigned>(port));
            std::printf("[host] 每收到一帧探针会广播三次（All / Where / Of），按 Ctrl+C 退出\n");
            std::fflush(stdout);

            const double t0 = orkdemo::NowMs();
            double lastReport = t0;
            while (!g_stop.load(std::memory_order_relaxed))
            {
                orkdemo::SleepMs(50);
                const double t = orkdemo::NowMs();
                if (t - lastReport >= 10000.0)
                {
                    lastReport = t;
                    std::printf("[host] up=%.1fs active=%zu rounds=%u "
                                "sent(All/Where/Of)=%llu/%llu/%llu inconsistent=%u\n",
                                (t - t0) / 1000.0,
                                framework->GetConnectionRegistry().Count(),
                                listener.rounds(),
                                listener.sentAll(), listener.sentWhere(), listener.sentOf(),
                                listener.inconsistent());
                    std::fflush(stdout);
                }
            }
        }
        else
        {
            rc = RunFanoutSelfTest(*framework, listener, clientCount,
                                   port, encryption, timeoutSec);
        }

        std::printf("\n[host] shutting down...\n");
        std::fflush(stdout);
        framework->Stop();
        framework->RegisterListener(nullptr);

        std::printf("[host] 累计: 接入=%u 断开=%u payload=%u 放大轮次=%u "
                    "sent(All/Where/Of)=%llu/%llu/%llu cryptoFail=%llu\n",
                    listener.connected(), listener.disconnected(), listener.payloads(),
                    listener.rounds(),
                    listener.sentAll(), listener.sentWhere(), listener.sentOf(),
                    listener.cryptoFails());
        std::printf("[host] bye\n");
        return rc;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[host] Fatal: %s\n", e.what());
        return 1;
    }
}
