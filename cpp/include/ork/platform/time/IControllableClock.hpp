#pragma once
#include "ork/platform/time/IGameClock.hpp"

class IControllableClock : public IGameClock
{
public:
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void SetTimeScale(double scale) = 0;
};
