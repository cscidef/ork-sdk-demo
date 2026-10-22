/**
 * @file main.cpp
 * @brief Ork SDK 调用教程示例（纯 Mock，无网络）
 *
 * Ork 是 Harmonia 核心通讯传输框架 SDK，对外提供：
 * - IHostListener（事件回调：连接 / 断开 / opaque 载荷 / 统计）
 * - ISender（发送：Send / Broadcast，opaque 字节）
 * - IConnectionRegistry（连接治理：Count / TryGet / ForEach / Snapshot / Disconnect）
 * - NetworkTransport（TCP bootstrap + UDP/KCP 传输）
 *
 * ============================================================
 * SDK 正式接入方式（业务方只需做 3 件事）
 * ============================================================
 *
 * 1. 实现 ork::sdk::IHostListener — 业务方的唯一事件入口
 *      OnConnected(conn)                                   — 连接就绪
 *      OnDisconnected(conn, reason)                        — 连接断开
 *      OnPayload(conn, channel, span)                      — opaque 入站载荷
 *      OnStats(stats)                                      — 周期网络统计
 *
 *    玩家身份、Join/Leave、房间状态、游戏 Tick 等业务语义由业务方在
 *    OnPayload 之上自行实现（参考 cpp/demos/03_cpp_host 的宿主实现）。
 *
 * 2. 通过 ork::sdk::ISender 发送数据 — 框架提供的发送能力
 *      framework->GetSender().Send(conn, payload, channel)
 *      framework->GetSender().Broadcast(targets, payload, channel)
 *
 * 3. 启动框架
 *      TransportConfig config;
 *      config.gamePort = 37564;  // Create() 必填
 *      auto framework = SessionFramework::Create(config);
 *      framework->RegisterListener(&myListener);
 *      framework->Start();
 *      // pump 由内部线程驱动；退出时 framework->Stop()
 *
 * ============================================================
 * 本 Demo 说明
 * ============================================================
 *
 * 本 Demo 使用 Mock 实现（DemoFramework / DemoSender），
 * 不依赖网络层和 Proto，纯本地模拟运行。
 * 目的是展示 IHostListener 的实现方式和回调语义。
 *
 * 正式使用时，替换 DemoFramework 为真实的 SessionFramework：
 *    SessionFramework::Create(config)  // 内部集成 NetworkTransport
 *
 * 关键文件：
 * - cpp/include/ork/sdk/sdk/session_interface.hpp — SDK 核心接口
 * - cpp/demos/01_echo_server/main.cpp             — Create() 真实网络最小宿主
 * - cpp/demos/03_cpp_host/main.cpp                — 宿主侧业务接入参考
 *
 * 迁移自 harmonia_ork source/examples/tutorial/sdk_demo.cpp（2026-10）。
 */

#include "ork/sdk/sdk/session_interface.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ====================================================================
// 辅助日志（Demo 用，不依赖框架 Logger）
// ====================================================================

#define DEMO_LOG(fmt, ...) printf("[DEMO] " fmt "\n", ##__VA_ARGS__)

// 业务侧自定义玩家 ID（core 已不感知 PlayerId，由宿主自行定义身份类型）
using PlayerId = uint32_t;

// ====================================================================
// 步骤 1：实现 ork::sdk::IHostListener — 业务方的核心工作
//
// 正式接入时，业务方只需实现此接口，框架在 pump 线程回调。
// DemoRoomListener 展示一个最简的"房间"实现：
// - 维护 conn → 玩家表（业务状态自持，core 不感知玩家）
// - 在 OnPayload 中解析自定义二进制命令
// ====================================================================

class DemoRoomListener final : public ork::sdk::IHostListener
{
public:
    // ---- 连接生命周期 ----

    void OnConnected(ConnectionId connectionId) override
    {
        DEMO_LOG("Client %u connected", connectionId);
        connectedClients_[connectionId] = true;
    }

    void OnDisconnected(ConnectionId connectionId, DisconnectReason reason) override
    {
        DEMO_LOG("Client %u disconnected (reason=%s)", connectionId, ToString(reason));
        connectedClients_.erase(connectionId);

        // 业务侧清理玩家数据
        auto it = clientToPlayer_.find(connectionId);
        if (it != clientToPlayer_.end())
        {
            players_.erase(it->second);
            clientToPlayer_.erase(it);
        }
    }

    // ---- opaque 载荷（业务协议由宿主自定义） ----

    void OnPayload(ConnectionId connectionId,
                   ork::sdk::Channel channel,
                   std::span<const uint8_t> payload) override
    {
        const char* channelName =
            channel == ork::sdk::Channel::Reliable ? "Reliable" : "Unreliable";

        if (channel != ork::sdk::Channel::Reliable || payload.size() < 4)
        {
            // Demo: UDP 高频数据（位姿等），静默更新
            if (payload.size() >= 12)
            {
                auto it = clientToPlayer_.find(connectionId);
                if (it != clientToPlayer_.end())
                {
                    std::memcpy(&players_[it->second].x, payload.data() + 0, 4);
                    std::memcpy(&players_[it->second].y, payload.data() + 4, 4);
                    std::memcpy(&players_[it->second].z, payload.data() + 8, 4);
                }
            }
            return;
        }

        DEMO_LOG("Payload from client %u, %zu bytes, %s",
                 connectionId, payload.size(), channelName);

        // Demo: 解析自定义命令（cmdType = 前 4 字节小端）
        uint32_t cmdType;
        std::memcpy(&cmdType, payload.data(), sizeof(cmdType));

        switch (cmdType)
        {
        case 0:  // Join 命令（业务自定义，core 不参与）
        {
            const PlayerId playerId = nextPlayerId_++;
            PlayerInfo info;
            info.playerId = playerId;
            players_[playerId] = info;
            clientToPlayer_[connectionId] = playerId;
            DEMO_LOG("Client %u joined as Player %u", connectionId, playerId);
            break;
        }
        case 1:  // 移动命令
            if (payload.size() >= 16)
            {
                float x, y, z;
                std::memcpy(&x, payload.data() + 4, 4);
                std::memcpy(&y, payload.data() + 8, 4);
                std::memcpy(&z, payload.data() + 12, 4);
                auto it = clientToPlayer_.find(connectionId);
                if (it != clientToPlayer_.end())
                {
                    players_[it->second].x = x;
                    players_[it->second].y = y;
                    players_[it->second].z = z;
                    DEMO_LOG("Player %u moved to (%.1f, %.1f, %.1f)",
                             it->second, x, y, z);
                }
            }
            break;
        case 2:  // 聊天命令
            DEMO_LOG("Chat from client %u: %.*s", connectionId,
                     static_cast<int>(payload.size() - 4),
                     reinterpret_cast<const char*>(payload.data() + 4));
            break;
        default:
            DEMO_LOG("Unknown command type %u from client %u", cmdType, connectionId);
            break;
        }
    }

    // ---- 网络统计 ----

    void OnStats(const ork::sdk::OrkStats& stats) override
    {
        DEMO_LOG("Stats: active=%llu recv=%llu send=%llu pump=%llu lag=%llu",
                 static_cast<unsigned long long>(stats.activeConnections),
                 static_cast<unsigned long long>(stats.recvPackets),
                 static_cast<unsigned long long>(stats.sendPackets),
                 static_cast<unsigned long long>(stats.pumpTickCount),
                 static_cast<unsigned long long>(stats.pumpLagCount));
    }

    // ---- Demo 辅助方法 ----

    size_t GetOnlinePlayerCount() const { return players_.size(); }
    size_t GetConnectedCount() const { return connectedClients_.size(); }
    uint32_t GetPayloadCount() const { return payloadCount_; }

    struct PlayerInfo
    {
        PlayerId playerId;
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    const std::unordered_map<PlayerId, PlayerInfo>& GetPlayers() const { return players_; }

private:
    std::unordered_map<ConnectionId, bool> connectedClients_;
    std::unordered_map<ConnectionId, PlayerId> clientToPlayer_;
    std::unordered_map<PlayerId, PlayerInfo> players_;
    PlayerId nextPlayerId_ = 100;
    uint32_t payloadCount_ = 0;
};

// ====================================================================
// 步骤 2：ISender Mock — 演示发送接口的调用语义
//
// 正式使用时，不需要自己实现 ISender。
// 框架通过 SessionFramework::GetSender() 提供 ork::sdk::Sender 实例，
// 业务方在任意线程调用 Send / Broadcast 即可。
// ====================================================================

class DemoSender final : public ork::sdk::ISender
{
public:
    ork::sdk::SendResult Send(ConnectionId conn,
                              std::span<const uint8_t> payload,
                              ork::sdk::Channel channel) override
    {
        const char* channelName = channel == ork::sdk::Channel::Reliable ? "Reliable" : "Unreliable";
        DEMO_LOG("Send(conn=%u, %zu bytes, %s)", conn, payload.size(), channelName);
        sentCount_++;
        return ork::sdk::SendResult::Ok;
    }

    ork::sdk::BroadcastReport Broadcast(const ork::sdk::ConnectionSet& targets,
                                        std::span<const uint8_t> payload,
                                        ork::sdk::Channel channel) override
    {
        const char* channelName = channel == ork::sdk::Channel::Reliable ? "Reliable" : "Unreliable";
        DEMO_LOG("Broadcast(kind=%d, %zu bytes, %s)",
                 static_cast<int>(targets.kind()), payload.size(), channelName);
        sentCount_++;
        return ork::sdk::BroadcastReport{.attempted = 1, .sent = 1};
    }

    uint32_t GetSentCount() const { return sentCount_; }

private:
    uint32_t sentCount_ = 0;
};

// ====================================================================
// 步骤 3：DemoFramework Mock — 模拟框架的事件注入
//
// 正式使用时，替换为真实 SessionFramework：
//   auto framework = SessionFramework::Create(config);
//   framework->RegisterListener(&listener);
//   framework->Start();
//
// DemoFramework 的 SimulateXxx 方法用于本地注入事件，
// 真实框架中这些事件来自网络 pump 线程（见 host_listener.hpp 线程契约）。
// ====================================================================

class DemoFramework
{
public:
    explicit DemoFramework(const TransportConfig& config)
        : config_(config)
    {
    }

    void RegisterListener(ork::sdk::IHostListener* listener)
    {
        listener_ = listener;
    }

    ork::sdk::ISender& GetSender() { return sender_; }

    void Start()
    {
        running_ = true;
        DEMO_LOG("Framework started (port=%u pumpRate=%uHz kcp=%s)",
                 config_.gamePort, config_.pumpRate, config_.enableKcp ? "true" : "false");
    }

    void Stop()
    {
        running_ = false;
        DEMO_LOG("Framework stopped");
    }

    // ---- 模拟事件注入（测试用） ----

    void SimulateConnected(ConnectionId connId)
    {
        if (listener_)
            listener_->OnConnected(connId);
    }

    void SimulateDisconnected(ConnectionId connId, DisconnectReason reason)
    {
        if (listener_)
            listener_->OnDisconnected(connId, reason);
    }

    void SimulatePayload(ConnectionId connId,
                         ork::sdk::Channel channel,
                         const uint8_t* data, size_t len)
    {
        if (listener_)
            listener_->OnPayload(connId, channel, std::span<const uint8_t>(data, len));
    }

    void SimulateStats(const ork::sdk::OrkStats& stats)
    {
        if (listener_)
            listener_->OnStats(stats);
    }

private:
    TransportConfig config_;
    bool running_ = false;
    ork::sdk::IHostListener* listener_ = nullptr;
    DemoSender sender_;
};

// ====================================================================
// 步骤 4：运行 Demo — 展示完整的会话生命周期
//
// 对应正式使用流程：
//   1. 配置 TransportConfig（pumpRate、enableKcp 等）
//   2. 创建框架 → RegisterListener → Start
//   3. 客户端通过网络连接（Demo 中用 SimulateXxx 模拟）
//   4. 业务方在 OnPayload 中处理自定义协议
//   5. 业务方通过 GetSender() 发送数据
//   6. Stop 关闭框架
// ====================================================================

// 辅助：构建 Join 命令（cmdType=0）
std::vector<uint8_t> BuildJoinCommand()
{
    std::vector<uint8_t> buf(4);
    const uint32_t cmdType = 0;
    std::memcpy(buf.data(), &cmdType, 4);
    return buf;
}

// 辅助：构建移动命令（cmdType=1, x, y, z）
std::vector<uint8_t> BuildMoveCommand(float x, float y, float z)
{
    std::vector<uint8_t> buf(16);
    uint32_t cmdType = 1;
    std::memcpy(buf.data(), &cmdType, 4);
    std::memcpy(buf.data() + 4, &x, 4);
    std::memcpy(buf.data() + 8, &y, 4);
    std::memcpy(buf.data() + 12, &z, 4);
    return buf;
}

// 辅助：构建位姿数据（x, y, z）
std::vector<uint8_t> BuildPoseData(float x, float y, float z)
{
    std::vector<uint8_t> buf(12);
    std::memcpy(buf.data(), &x, 4);
    std::memcpy(buf.data() + 4, &y, 4);
    std::memcpy(buf.data() + 8, &z, 4);
    return buf;
}

int main()
{
    DEMO_LOG("========================================");
    DEMO_LOG("  Ork SDK Demo (Phase 3.4 transport-only)");
    DEMO_LOG("========================================");

    // 1. 配置（仅传输字段；房间/tick 等业务配置由宿主自持）
    TransportConfig config;
    config.gamePort = 37564;
    config.pumpRate = 30;
    config.enableKcp = true;

    // A1.1：本地模拟 demo —— 显式明文开发模式（仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=
    // ON 构建可用，需两端 loopback；OFF 构建下 Create 校验 fail closed）。
    // 生产接入走 TlsServerAuthenticated + 证书链/私钥（见 echo_server 的
    // network.yaml 装配与 ork_tls_bootstrap_test）。
    config.bootstrapSecurity =
        ork::platform::BootstrapSecurityMode::PlaintextDevelopment;

    // 2. 创建框架和 Listener
    DemoFramework framework(config);
    DemoRoomListener listener;
    framework.RegisterListener(&listener);
    framework.Start();

    // 3. 模拟客户端连接
    DEMO_LOG("--- 模拟 3 个客户端连接 ---");
    framework.SimulateConnected(1);
    framework.SimulateConnected(2);
    framework.SimulateConnected(3);

    // 4. 模拟 Join（业务自定义命令，cmdType=0）
    DEMO_LOG("--- 模拟客户端 Join（业务协议） ---");
    auto joinCmd = BuildJoinCommand();
    framework.SimulatePayload(1, ork::sdk::Channel::Reliable, joinCmd.data(), joinCmd.size());
    framework.SimulatePayload(2, ork::sdk::Channel::Reliable, joinCmd.data(), joinCmd.size());
    framework.SimulatePayload(3, ork::sdk::Channel::Reliable, joinCmd.data(), joinCmd.size());

    // 5. 模拟可靠命令（移动）
    DEMO_LOG("--- 模拟可靠命令（移动） ---");
    auto moveCmd = BuildMoveCommand(10.5f, 0.0f, 5.3f);
    framework.SimulatePayload(1, ork::sdk::Channel::Reliable, moveCmd.data(), moveCmd.size());

    auto moveCmd2 = BuildMoveCommand(-3.0f, 2.0f, 8.0f);
    framework.SimulatePayload(2, ork::sdk::Channel::Reliable, moveCmd2.data(), moveCmd2.size());

    // 6. 模拟不可靠数据（UDP 位姿）
    DEMO_LOG("--- 模拟不可靠数据（UDP 位姿） ---");
    auto poseData = BuildPoseData(12.0f, 0.0f, 6.0f);
    framework.SimulatePayload(1, ork::sdk::Channel::Unreliable, poseData.data(), poseData.size());

    // 7. 发送数据（ISender）
    DEMO_LOG("--- 通过 ISender 发送 ---");
    framework.GetSender().Send(1, joinCmd, ork::sdk::Channel::Reliable);
    framework.GetSender().Broadcast(
        ork::sdk::ConnectionSet::All(), poseData, ork::sdk::Channel::Unreliable);

    // 8. 模拟统计上报
    DEMO_LOG("--- 模拟 OnStats ---");
    ork::sdk::OrkStats stats;
    stats.activeConnections = 3;
    stats.recvPackets = 42;
    stats.sendPackets = 7;
    stats.pumpTickCount = 900;
    framework.SimulateStats(stats);

    // 9. 模拟客户端断开
    DEMO_LOG("--- 模拟客户端断开 ---");
    framework.SimulateDisconnected(2, DisconnectReason::PeerClosed);

    // 10. 打印最终状态
    DEMO_LOG("--- 最终状态 ---");
    DEMO_LOG("Connected clients: %zu", listener.GetConnectedCount());
    DEMO_LOG("Online players: %zu", listener.GetOnlinePlayerCount());

    for (const auto& [pid, info] : listener.GetPlayers())
    {
        DEMO_LOG("Player %u: pos=(%.1f, %.1f, %.1f)", pid, info.x, info.y, info.z);
    }

    // 11. 停止
    framework.Stop();

    DEMO_LOG("========================================");
    DEMO_LOG("  Demo 完成!");
    DEMO_LOG("========================================");

    return 0;
}
