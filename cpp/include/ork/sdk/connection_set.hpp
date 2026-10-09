/**
 * @file connection_set.hpp
 * @brief 广播目标连接集合的三种表达：All / Of / Where
 *
 * 设计要点：
 * - Of 拥有（拷贝）ID 列表，不持有调用者的临时 span，生命周期安全。
 * - Where 的谓词通过 std::function 持有，调用方须保证谓词在 Broadcast 返回前有效；
 *   由于 Broadcast 是同步调用，std::function 的拷贝/移动语义已满足安全要求。
 * - All 无状态，单例即可。
 */

#pragma once

#include "ork/platform/types.hpp"
#include <functional>
#include <span>
#include <vector>

namespace ork::sdk {

class ConnectionSet
{
public:
    enum class Kind : uint8_t {
        All,    ///< 所有活跃连接
        Of,     ///< 指定的连接 ID 列表（拥有副本）
        Where   ///< 谓词筛选
    };

    /// 匹配所有活跃连接
    static ConnectionSet All() noexcept
    {
        return ConnectionSet(Kind::All);
    }

    /// 指定连接 ID 列表：拷贝一份，不持有调用者 span
    static ConnectionSet Of(std::span<const ConnectionId> ids)
    {
        ConnectionSet cs(Kind::Of);
        cs.ids_.assign(ids.begin(), ids.end());
        return cs;
    }

    /// 谓词筛选（std::function 拥有谓词副本）
    static ConnectionSet Where(std::function<bool(ConnectionId)> predicate)
    {
        ConnectionSet cs(Kind::Where);
        cs.predicate_ = std::move(predicate);
        return cs;
    }

    Kind kind() const noexcept { return kind_; }

    /// Of 模式的 ID 列表（仅 kind==Of 时有效）
    const std::vector<ConnectionId>& ids() const noexcept { return ids_; }

    /// Where 模式的谓词（仅 kind==Where 时有效）
    const std::function<bool(ConnectionId)>& predicate() const noexcept { return predicate_; }

    /// 对给定 connId 判断是否匹配（All 始终 true）
    bool Matches(ConnectionId connId) const
    {
        switch (kind_)
        {
        case Kind::All:
            return true;
        case Kind::Of:
            for (auto id : ids_)
                if (id == connId) return true;
            return false;
        case Kind::Where:
            return predicate_ ? predicate_(connId) : false;
        }
        return false;
    }

private:
    explicit ConnectionSet(Kind k) : kind_(k) {}

    Kind kind_;
    std::vector<ConnectionId> ids_;
    std::function<bool(ConnectionId)> predicate_;
};

} // namespace ork::sdk
