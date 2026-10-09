#pragma once
#include "ork/platform/domain_data/game_config.hpp"
#include "ork/platform/time/IReadOnlyClock.hpp"
#include <string>
#include <vector>
// for runtime level components only
struct RuntimeContext
{
    IReadOnlyClock& clock;

    std::string roomName;
    std::vector<RoomMemberInfo> members;
    GameConfig gameConfig;
};
