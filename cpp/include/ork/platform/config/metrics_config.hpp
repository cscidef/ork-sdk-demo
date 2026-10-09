#pragma once
#include <stdexcept>

struct MetricsConfig
{
    // Phase 4.5：默认成员初始化器——防御 server.yaml includes 未列出 metrics.yaml
    // 时读到未初始化字段（UB）。默认值与 config/metrics.yaml 一致。
    int tick_lag_count_threshold{5};
    int worker_queue_length_threshold{1000};

    void Validate() const
    {
        if (tick_lag_count_threshold < 0)
            throw std::runtime_error("MetricsConfig: invalid tick_lag_count_threshold");
        if (worker_queue_length_threshold < 0)
            throw std::runtime_error("MetricsConfig: invalid worker_queue_length_threshold");
    }
};
