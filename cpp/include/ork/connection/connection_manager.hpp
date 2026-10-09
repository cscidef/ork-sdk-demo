/**
 * @file connection_manager.hpp
 * @brief 传输层连接表管理（Phase 4.2: strand 私有 + 快照发布）
 *
 * ConnectionManager 维护所有 TCP bootstrap 建立的 Logical Connection，
 * 负责 sessionToken 分配、UDP endpoint 绑定、加密状态初始化，以及
 * 供 IConnectionRegistry 使用的只读查询与空闲检测。
 *
 * Phase 4.2 变更：
 * - connections_ 为 strand 私有（所有写操作在 NetworkPump strand 内执行）
 * - 新增快照发布机制：strand 定期 PublishSnapshot()，外部只读 GetSnapshot()
 * - FindMutable 仅限 strand 内使用
 * - 外部查询（Count/TryGet/ForEach/Snapshot）读 atomic shared_ptr 快照
 */

#pragma once
#include "ork/connection/endpoint_key.hpp"
#include "ork/connection/logical_connection.hpp"
#include "ork/platform/context/network_context.hpp"
#include "ork/transport/data/network_defs.hpp"
#include "ork/platform/utils/secure_random.hpp"
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

/**
 * @brief B1：UDP Handshake V2 绑定状态机判定结果（Rev 3 §5.7）
 *
 * 由 HandleUdpHandshakeV2 在 strand 内原子判定并返回；调用方
 * （NetworkTransport::OnUdpHandshake）据此区分“首次绑定（发 Connected）”
 * 与“幂等重试（仅重发 ACK）”，以及各拒绝类别对应的 Metrics 计数。
 */
enum class UdpHandshakeV2BindResult
{
    FirstBind,        ///< 首次合法：原子提交 endpoint + nonce + 水位 + Established
    AckRetry,         ///< 同 nonce 更大 counter：仅推进水位，重发 ACK，不重复 Connected
    ReplayRejected,   ///< 同 nonce，counter <= 水位：静默丢弃
    NonceMismatch,    ///< 不同 nonce：B1 静默拒绝（B2 才 challenge-response）
    EndpointMismatch, ///< 已绑定 endpoint 且来源不同：拒绝换绑
    UnknownConnection ///< connectionId 未命中（调用方在 HMAC 前已查表，防御性保留）
};

// =============================================================================
// Phase 4.2: 连接快照（只读，供外部线程查询）
// =============================================================================

/**
 * @brief 单条连接的只读快照条目
 *
 * 不含 cipher 可变状态（Seal/Open 全在 strand 内执行）；
 * 不含 tcp shared_ptr（外部不需要操作 socket）。
 */
struct ConnectionSnapshotEntry
{
    ConnectionId id = 0;
    SessionToken sessionToken = 0;
    ConnectionState state = ConnectionState::Disconnected;
    std::optional<asio::ip::udp::endpoint> udp;
    TimePoint lastActive = 0;
    bool encryptionActive = false;
};

/**
 * @brief 连接表只读快照（atomic shared_ptr 交换发布）
 */
struct ConnectionSnapshot
{
    std::unordered_map<ConnectionId, ConnectionSnapshotEntry> connections;
    int64_t publishedAtUs = 0;

    /// 活跃连接数（state != Disconnected）
    size_t ActiveCount() const
    {
        size_t count = 0;
        for (const auto& [id, entry] : connections)
        {
            (void)id;
            if (entry.state != ConnectionState::Disconnected)
                ++count;
        }
        return count;
    }
};

// =============================================================================
// ConnectionManager
// =============================================================================

class ConnectionManager
{
public:
    ConnectionManager(RuntimeContext& runtimeContext)
        : networkContext_(runtimeContext)
    {
    }

    void ConfigureCipher(bool enable_encryption, int bootstrap_ttl_sec, bool cipher_debug_log);

    // ===== TCP 生命周期（strand 内调用） =====

    /** @brief TCP 接入：分配 connectionId / sessionToken，初始化 cipher，写入连接表 */
    ConnectionId OnTcpConnected(std::shared_ptr<TcpConnection> tcp);

    /** @brief TCP 断开：释放 tcp 句柄，state 置 Disconnected */
    void OnTcpDisconnected(ConnectionId id);

    /** @brief 同 token 的 TCP 重连：恢复 tcp 句柄并刷新 lastActive */
    bool RebindTcp(ConnectionId id, SessionToken token, std::shared_ptr<TcpConnection> tcp);

    // ===== UDP 生命周期（strand 内调用） =====

    /**
     * @brief B1：UDP Handshake V2 首次绑定 / 幂等重试状态机（strand 内原子）
     *
     * 调用前提（Rev 3 §5.7 处理顺序）：调用方已完成 exact 42B / type / version
     * 基础校验、connectionId 查表、Bootstrap TTL 检查与 authTag 常量时间验证；
     * 本方法只承担防重放与 endpoint/状态原子提交：
     *   - 首次合法：endpoint + current nonce + highest counter + Established
     *     在同一次 strand 调用内提交（无中间态可观察）；
     *   - 同 nonce 更大 counter：仅推进水位（不重建业务状态，Connected 不重复）；
     *   - 同 nonce 相同/更小 counter：拒绝（重放）；
     *   - 不同 nonce：拒绝（B1 无换绑）；
     *   - 已绑定 endpoint 且来源不同：拒绝（即便 nonce/tag 合法也不得覆盖）。
     *
     * @param clientNonce 帧内 16B nonce（仅与连接内已提交值比较，不落日志）
     * @param counter     帧内 4B 计数器（BE 已解析）
     */
    UdpHandshakeV2BindResult HandleUdpHandshakeV2(
        ConnectionId id, const ork::platform::ClientNonce& clientNonce,
        uint32_t counter, const asio::ip::udp::endpoint& endpoint);

    /** @brief 校验 UDP 包来源 endpoint 是否与已绑定的一致 */
    bool ValidateUdpPacket(ConnectionId id, const asio::ip::udp::endpoint& endpoint);

    /** @brief 由 UDP endpoint 反查 connectionId（KCP 收包路由用，strand 内） */
    std::optional<ConnectionId> FindByUdpEndpoint(const asio::ip::udp::endpoint& endpoint) const;

    // ===== strand 内部操作 =====

    /**
     * @brief 按 connectionId 拷贝一份 Connection 快照（strand 内用）
     */
    bool Find(ConnectionId id, Connection& connection) const
    {
        auto it = connections_.find(id);
        if (it == connections_.end())
            return false;
        connection = it->second;
        return true;
    }

    /** @brief 按 connectionId 获取可写指针（strand 内用：cipher Seal/Open、TouchActivity） */
    Connection* FindMutable(ConnectionId id)
    {
        auto it = connections_.find(id);
        if (it == connections_.end())
            return nullptr;
        return &it->second;
    }

    /**
     * @brief 遍历连接表（strand 内用）
     */
    void ForEachConnection(const std::function<void(ConnectionId, const Connection&)>& fn) const
    {
        for (const auto& [id, conn] : connections_)
        {
            fn(id, conn);
        }
    }

    /**
     * @brief 刷新连接的最后活跃时间（strand 内调用）
     */
    void TouchActivity(ConnectionId id)
    {
        auto* conn = FindMutable(id);
        if (!conn)
            return;
        conn->lastActive = networkContext_.clock.Now();
    }

    /**
     * @brief 当前单调时钟时间点（strand 内用：空闲检测 now 基准）
     */
    TimePoint Now() const
    {
        return networkContext_.clock.Now();
    }

    /**
     * @brief 收集超过空闲阈值的连接 ID 列表（strand 内调用）
     */
    std::vector<ConnectionId> CollectIdleConnections(TimePoint now, Duration idleTimeoutUs) const;

    /**
     * @brief A0.2：收集 TcpOnly 停留超时的连接 ID 列表（strand 内调用）
     *
     * 判定基于 Connection::tcpOnlySince（进入 TcpOnly 的时刻，与 lastActive
     * 无关）：bootstrap 后不发任何 TCP 流量的客户端不会被误判为“活跃”。
     * 由 NetworkTransport::HostPump 定期调用，超时连接经 DisconnectImpl 回收
     * （不采用“腾挪最早 TcpOnly”策略，避免 LRU 误杀）。
     */
    std::vector<ConnectionId> CollectTcpOnlyTimeouts(TimePoint now, Duration timeoutUs) const;

    // ===== A0.2：接纳门禁计数（strand 内更新，原子发布读取） =====

    /** @brief TCP 活跃连接数（state != Disconnected；原子读，任意线程安全） */
    size_t ActiveTcpCount() const
    {
        return activeTcpCount_.load(std::memory_order_acquire);
    }

    /** @brief 未完成 Bootstrap（TcpOnly/UdpHandshaking）连接数（原子读，任意线程安全） */
    size_t PendingTcpOnlyCount() const
    {
        return pendingTcpOnlyCount_.load(std::memory_order_acquire);
    }

    // ===== Phase 4.2: 快照发布与外部只读访问 =====

    /**
     * @brief 发布连接表快照（strand 内定期调用）
     *
     * 构造新的 ConnectionSnapshot，通过 atomic store 替换 publishedSnapshot_。
     * 外部线程通过 GetSnapshot() 读取最新快照。
     */
    void PublishSnapshot();

    /**
     * @brief 获取最新只读快照（外部线程安全）
     *
     * 使用 std::atomic_load 读取 shared_ptr，无锁。
     */
    std::shared_ptr<const ConnectionSnapshot> GetSnapshot() const
    {
        return std::atomic_load(&publishedSnapshot_);
    }

    // ===== 外部只读查询（基于快照，线程安全） =====

    /** @brief 活跃连接数（读快照） */
    size_t CountTransportConnections(bool excludeDisconnected = true) const
    {
        auto snapshot = GetSnapshot();
        if (!snapshot)
            return 0;
        if (!excludeDisconnected)
            return snapshot->connections.size();
        return snapshot->ActiveCount();
    }

    /** @brief 按 connectionId 查询快照条目（外部线程安全） */
    bool FindSnapshotEntry(ConnectionId id, ConnectionSnapshotEntry& out) const
    {
        auto snapshot = GetSnapshot();
        if (!snapshot)
            return false;
        auto it = snapshot->connections.find(id);
        if (it == snapshot->connections.end())
            return false;
        out = it->second;
        return true;
    }

    /** @brief 遍历快照中所有活跃连接（外部线程安全） */
    void ForEachSnapshotConnection(const std::function<void(ConnectionId, const ConnectionSnapshotEntry&)>& fn) const
    {
        auto snapshot = GetSnapshot();
        if (!snapshot)
            return;
        for (const auto& [id, entry] : snapshot->connections)
        {
            fn(id, entry);
        }
    }

private:
    ConnectionId GenerateConnectionId()
    {
        return nextConnectionId_++;
    }

    /**
     * @brief A0.2：状态转移 + 计数维护（strand 内；幂等，新旧行同不动作）
     *
     * 统一维护 activeTcpCount_（state != Disconnected）与
     * pendingTcpOnlyCount_（TcpOnly/UdpHandshaking）两个原子计数，
     * 进入 TcpOnly 时刷新 tcpOnlySince。对称加减保证不重复减、不下溢。
     */
    void TransitionTo(Connection& conn, ConnectionState next);

    /**
     * @brief 生成 sessionToken（连接关联字段，非安全凭证 —— 设计文档 #21）
     *
     * A0.1：生成源从 std::mt19937 切换为 libsodium CSPRNG。
     *   - 非零：既有约定（0 表示未赋值/无效），由 U32NonZero 内建保证；
     *   - 唯一性：不查重 —— 连接身份由 connectionId 唯一确定，token 相撞
     *     仅意味着两连接的关联值相同；RebindTcp / HandleUdpHandshakeV2 均先按
     *     connectionId 查表，正确性不受影响（B1 起握手认证由 authTag 承担，
     *     token 不再参与 UDP 绑定判定）；
     *   - 强度：CSPRNG 消除可预测性是纵深防御，32 位不构成枚举安全边界，
     *     认证由 OrkCipher AEAD 的 AAD 绑定（28B，含 conn+token）承担。
     */
    SessionToken GenerateSessionToken()
    {
        return ork::platform::SecureRandom::U32NonZero();
    }

    // ===== Phase 4.4: endpoint 反向索引维护（strand 内，与 conn.udp 原子同步） =====

    /** @brief 写入 endpoint→id 索引（重复 endpoint 时后写者覆盖，last-writer-wins） */
    void IndexEndpoint(ConnectionId id, const asio::ip::udp::endpoint& endpoint);

    /**
     * @brief 删除 endpoint 索引项（仅当当前映射确为 expectedId 时才删）
     *
     * 守卫 expectedId 可避免重复 endpoint 场景下误删更新的绑定。
     */
    void UnindexEndpoint(const asio::ip::udp::endpoint& endpoint, ConnectionId expectedId);

    /** @brief 按连接当前 udp 绑定删除其索引项（断开/改绑前调用） */
    void UnindexConnection(const Connection& conn);

private:
    /// strand 私有连接表（所有写操作在 strand 内，无锁）
    std::unordered_map<ConnectionId, Connection> connections_;

    /// Phase 4.4: endpoint→connectionId 反向索引（strand 私有，与 conn.udp 同步维护）
    /// 把 FindByUdpEndpoint / ValidateUdpPacket 从 O(N) 遍历降为 O(1) 哈希查找。
    std::unordered_map<ork::connection::EndpointKey, ConnectionId,
                       ork::connection::EndpointKeyHash> endpointIndex_;

    NetworkContext networkContext_;
    bool encryptionRequired_{false};
    int bootstrapTtlSec_{300};
    std::atomic<ConnectionId> nextConnectionId_{1};

    /// A0.2：strand 内更新（TransitionTo），原子发布供 accept 门禁读取
    std::atomic<size_t> activeTcpCount_{0};
    std::atomic<size_t> pendingTcpOnlyCount_{0};

    /// Phase 4.2: 原子发布的只读快照
    std::shared_ptr<const ConnectionSnapshot> publishedSnapshot_;
};
