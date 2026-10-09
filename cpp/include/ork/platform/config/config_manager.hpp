#pragma once
#include "ork/platform/config/log_config.hpp"
#include "ork/platform/config/metrics_config.hpp"
#include "ork/platform/config/network_config.hpp"
#include "ork/platform/config/room_config.hpp"
#include "ork/platform/config/sync_config.hpp"
#include <string>
#include <vector>

class ConfigManager
{
public:
    static ConfigManager& Instance();

    void Load(const std::string& configDir);

    /**
     * @brief 解析 server.yaml 的 includes 清单（Phase 4.5 配置权威化）
     *
     * 读取 <configDir>/server.yaml 的 `includes` 序列，对每项做安全校验：
     * - 拒绝绝对路径与目录穿越（含 `..` 分量）；
     * - 拒绝非 .yaml 扩展名；
     * - 拒绝重复项；
     * - 拒绝缺失文件。
     * 任一校验失败抛 std::runtime_error。
     *
     * 回退路径（明确的错误恢复，仅 WARN 不抛）：
     * - server.yaml 不存在 / 无法解析；
     * - server.yaml 无 `includes` 序列或为空。
     * 上述情况返回 DefaultIncludeList()。
     *
     * @param configDir  配置根目录
     * @return 按 includes 顺序（或默认顺序）的文件名列表（相对 configDir）
     */
    static std::vector<std::string> ResolveIncludeList(const std::string& configDir);

    /**
     * @brief 默认包含清单（server.yaml 缺失/无 includes 时的回退）
     *
     * 与 Phase 4.5 之前硬编码的五份配置一致。
     */
    static std::vector<std::string> DefaultIncludeList();

    const NetworkConfig& Network() const
    {
        return network_;
    }

    const RoomYamlConfig& Room() const
    {
        return room_;
    }

    const SyncConfig& Sync() const
    {
        return sync_;
    }

    const LogConfig& Log() const
    {
        return log_;
    }

    const MetricsConfig& Metrics() const
    {
        return metrics_;
    }

private:
    ConfigManager() = default;

    NetworkConfig network_;
    RoomYamlConfig room_;
    SyncConfig sync_;
    LogConfig log_;
    MetricsConfig metrics_;
};
