#pragma once

/**
 * @file disconnect_reason.hpp
 * @brief 连接断开原因（纯传输语义，Phase 3.4）
 *
 * 归属：连接、传输层。与玩家/房间/Join 等业务语义无关。
 * 由 IHostListener::OnDisconnected 上报宿主，亦用于主动断开时指定原因。
 */

#include <cstdint>
#include <cstddef>

enum class DisconnectReason : uint8_t
{
    /// 宿主主动断开（例如运维踢人）
    Kicked = 0,
    /// 空闲超时（TransportConfig::connectionIdleTimeoutSec 触发）
    IdleTimeout = 1,
    /// 解密失败（AEAD 校验不过 / 重放）。预留：当前传输层仅告警计数，Phase 4 接线断开
    CryptoFailed = 2,
    /// 协议错误（帧格式非法 / 未知包类型）。预留：当前传输层仅告警
    ProtocolError = 3,
    /// Bootstrap 阶段失败（TCP bootstrap 异常 / 密钥下发失败）。预留
    BootstrapFailed = 4,
    /// 对端关闭（TCP FIN / 对端进程退出）
    PeerClosed = 5,
};

/// DisconnectReason 枚举成员数量（用于 OrkStats / Metrics 的按原因计数数组）。
/// 新增枚举值时须同步更新此常量，保证计数数组覆盖全部原因。
inline constexpr std::size_t kDisconnectReasonCount = 6;

/// 将 DisconnectReason 安全映射为计数数组下标（越界钳制到最后一个槽位）。
inline constexpr std::size_t DisconnectReasonIndex(DisconnectReason reason) noexcept
{
    const auto v = static_cast<std::size_t>(reason);
    return v < kDisconnectReasonCount ? v : kDisconnectReasonCount - 1;
}

/// 人类可读名（日志用）
constexpr const char* ToString(DisconnectReason reason) noexcept
{
    switch (reason)
    {
    case DisconnectReason::Kicked:
        return "kicked";
    case DisconnectReason::IdleTimeout:
        return "idle_timeout";
    case DisconnectReason::CryptoFailed:
        return "crypto_failed";
    case DisconnectReason::ProtocolError:
        return "protocol_error";
    case DisconnectReason::BootstrapFailed:
        return "bootstrap_failed";
    case DisconnectReason::PeerClosed:
        return "peer_closed";
    }
    return "unknown";
}
