#pragma once
#include "ork/platform/time/IGameClock.hpp"

class MockClock final : public IGameClock
{
public:
    explicit MockClock(TimePoint start = 0)
        : now_(start)
    {
    }

    TimePoint Now() const override
    {
        return now_;
    }

    void Advance(Duration delta) override
    {
        now_ += delta;
    }

    void Reset(TimePoint start = 0) override
    {
        now_ = start;
    }

private:
    TimePoint now_;
};
