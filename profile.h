#pragma once

#include "window_io.h"
#include <array>
#include <string_view>

enum class Ruleset : int { Normal, PvP, Roleplay, RoleplayPvP };
enum class Faction : int { Alliance, Horde };
enum class Body : int { Type1, Type2 };
enum class RetryMode : int { Loop, BattleNetRestart };
enum class CharacterClass : int { Warrior, Hunter, Mage, Rogue, Priest, Warlock, Paladin, Druid, Shaman };

inline constexpr std::array rulesets{L"Normal", L"PvP", L"Roleplay", L"Roleplay PvP"};
inline constexpr std::array factions{L"Alliance", L"Horde"};
inline constexpr std::array bodies{L"Body 1", L"Body 2"};
inline constexpr std::array retryModes{L"Loop mode", L"Battle.net Restart mode"};
inline constexpr std::array classes{L"Warrior", L"Hunter", L"Mage", L"Rogue", L"Priest",
                                    L"Warlock", L"Paladin", L"Druid", L"Shaman"};
inline constexpr std::array<std::array<const wchar_t*, 5>, 2> races{{
    {L"Human", L"Dwarf", L"Night Elf", L"Gnome", L"High Order Skyborne"},
    {L"Orc", L"Undead", L"Tauren", L"Troll", L"Windshaper Skyborne"},
}};
inline constexpr std::size_t maxNameLength = 12;

struct Profile {
    Ruleset ruleset = Ruleset::PvP;
    Faction faction = Faction::Horde;
    int race = 2;
    Body body = Body::Type1;
    CharacterClass characterClass = CharacterClass::Druid;
    std::wstring firstName, secondName;
    std::wstring battleNetLauncher, gameLauncher;
    bool muted = false;
    RetryMode retryMode = RetryMode::BattleNetRestart;
};

std::wstring CharacterSummary(const Profile& profile);
bool CanPlay(Faction faction, int race, CharacterClass characterClass);
std::wstring ValidateCharacter(const Profile& profile);
std::wstring ValidateProfile(const Profile& profile);
bool LoadProfile(Profile& profile);
void SaveProfile(const Profile& profile);
void FindInstallations(Profile& profile);
bool HasForeverInstallation(const std::wstring& launcher);

struct SessionProfile {
    int ruleset, faction, race, body, characterClass;
    wchar_t firstName[maxNameLength + 1], secondName[maxNameLength + 1];
    wchar_t battleNetLauncher[1024], gameLauncher[1024];
};

SessionProfile ToSessionProfile(const Profile& profile);
Profile FromSessionProfile(const SessionProfile& wire);
