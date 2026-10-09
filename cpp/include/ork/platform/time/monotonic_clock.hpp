#pragma once

#include "ork/platform/time/IReadOnlyClock.hpp"
#include "ork/platform/time/time_defs.hpp"

#include <chrono>

/**
 * @brief 生产用只读单调时钟（Phase 4.3）
 *
 * 直接读取 std::chrono::steady_clock，返回自 steady_clock epoch 起的微秒数。
 *
 * 取代旧的 GameClock：GameClock 是 IGameClock，需要 TickThread 每帧手动
 * Advance() 才会推进；Phase 4.3 消除 TickThread 后，连接空闲检测、快照
 * publishedAtUs、lastActive 均改由本时钟提供真实单调时间基准。
 *
 * 所有消费方仅使用差值语义（now - lastActive >= idleTimeout），
 * 因此绝对基准（epoch 起点）不影响正确性。
 */
class MonotonicClock final : public IReadOnlyClock
{
public:
    TimePoint Now() const override
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};
