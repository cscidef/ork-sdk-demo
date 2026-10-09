#pragma once
#include "ork/platform/time/time_defs.hpp"

class IReadOnlyClock
{
public:
    virtual ~IReadOnlyClock() = default;

    virtual TimePoint Now() const = 0;
};
