#pragma once
#include "ork/platform/time/IReadOnlyClock.hpp"
#include "ork/platform/time/time_defs.hpp"

class IGameClock : public IReadOnlyClock
{
public:
    virtual ~IGameClock() = default;

    /// forward only
    virtual void Advance(Duration delta) = 0;

    /// reset time for debug/replay purposes
    virtual void Reset(TimePoint start = 0) = 0;
};
