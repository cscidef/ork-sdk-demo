#pragma once
#include <stdexcept>

/// room.yaml 的解析结构（配置加载层；业务侧房间配置见 examples RoomConfig）
struct RoomYamlConfig {
    // Phase 4.5：默认成员初始化器——防御 includes 未列出 room.yaml 时读未初始化字段。
    int max_players{100};
    int heartbeat_timeout_ms{3000};

    void Validate() const {
        if (max_players <= 0)
            throw std::runtime_error("RoomYamlConfig: invalid max_players");

        if (heartbeat_timeout_ms <= 0)
            throw std::runtime_error("RoomYamlConfig: invalid heartbeat_timeout_ms");
    }
};
