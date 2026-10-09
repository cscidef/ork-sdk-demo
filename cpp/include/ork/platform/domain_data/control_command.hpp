#pragma once

#include "ork/platform/domain_data/game_config.hpp"
#include <functional>
#include <string>
#include <variant>

enum class ControlCommandType
{
    Shutdown,
    Pause,
    Resume,
    DumpState,
    KickPlayer,
    OpenRoom
};

struct KickPlayerCommand
{
    ControlCommandType type;
    std::string player_id;
    std::string reason;
};

struct OpenRoomCommand
{
    ControlCommandType type;
    std::string room_name;
    std::vector<RoomMemberInfo> members;
    GameConfig gameConfig;
};

struct NormalCommand
{
    ControlCommandType type;
};

using ControlCommand = std::variant<NormalCommand, KickPlayerCommand, OpenRoomCommand>;

using OnControlCommandReceivedCallback = std::function<void(const ControlCommand&)>;
