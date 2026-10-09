#pragma once
#include "ork/platform/time/IReadOnlyClock.hpp"
#include "ork/platform/time/time_defs.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>

using TimerId = uint64_t;

class TimerManager
{
public:
    using Callback = std::function<void()>;

    struct Timer
    {
        TimePoint triggerTime;
        TimerId id;
        Callback cb;
        bool cancelled{false};
    };

    struct CompareByTriggerTime
    {
        bool operator()(const Timer* a, const Timer* b) const
        {
            return a->triggerTime > b->triggerTime;
        }
    };

    explicit TimerManager(IReadOnlyClock& clock)
        : clock_(clock)
    {
    }

    ~TimerManager() = default;

    /// trigger at specific time point
    TimerId ScheduleAt(TimePoint triggerTime, Callback cb);

    /// trigger after delay
    TimerId ScheduleAfter(Duration delay, Callback cb);

    /// cancel timer (idempotent)
    void Cancel(TimerId id);

    /// call in Tick to trigger expired timers
    void Tick();

private:
    TimerId nextId_{0};

    IReadOnlyClock& clock_;

    std::priority_queue<Timer*, std::vector<Timer*>, CompareByTriggerTime> timers_;

    std::unordered_map<TimerId, std::unique_ptr<Timer>> timersMap_;
};
