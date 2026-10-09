#pragma once
#include <stdexcept>

struct SyncConfig
{
    // Phase 4.5：默认成员初始化器——防御 includes 未列出 sync.yaml 时读未初始化字段。
    // 默认值与 config/sync.yaml 一致。（部分字段已 reserved，见 sync.yaml 注释）
    int tick_rate_hz{50};
    int max_frame_time_ms{250};
    int delta_history_ticks{100};
    int full_snapshot_send_interval{50}; // in step

    void Validate() const
    {
        if (tick_rate_hz <= 0 || tick_rate_hz > 120)
            throw std::runtime_error("SyncConfig: invalid tick_rate_hz");
        if (max_frame_time_ms <= 0)
            throw std::runtime_error("SyncConfig: invalid max_frame_time_ms");
        if (delta_history_ticks <= 0)
            throw std::runtime_error("SyncConfig: invalid delta_history_ticks");
        if (full_snapshot_send_interval <= 0)
            throw std::runtime_error("SyncConfig: invalid full_snapshot_send_interval");
    }
};
