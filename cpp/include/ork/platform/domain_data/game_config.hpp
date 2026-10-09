#pragma once
#include <string>

struct GameConfig
{
    int gameTime;
};

struct UserInfo
{
    std::string playerName;
    int gender;
    std::string headPicUrl;
    int userCamp;
    int userPlace;
    int bulletSpeed;
    int bulletScale;
    int chargeSpeed;
    int skillStrength;
};

struct SkillInfo
{
    int skillId;
    int skillCooldown;
    int spellTime;
    int elementCount;
    int elementAngle;
    int elementCapacity;
};

struct HeroInfo
{
    int heroId;
    std::string heroName;
    std::string heroHeadPicUrl;
    int health;
    int attack;
    int critialAttackMultiplier;
    int speedBuf;
    int scaleBuf;
    int chargeSpeedBuf;
    SkillInfo skillInfo;
};

struct RoomMemberInfo
{
    std::string playerUUID;
    UserInfo userInfo;
    HeroInfo heroInfo;
};
