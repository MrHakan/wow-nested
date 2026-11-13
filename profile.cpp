#include "profile.h"
#include <shlobj.h>
#include <dpapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

namespace {

constexpr unsigned Mask(std::initializer_list<CharacterClass> choices) {
    unsigned mask = 0;
    for (auto choice : choices)
        mask |= 1u << int(choice);
    return mask;
}

using enum CharacterClass;

// The race order follows the two columns in the English client.
constexpr std::array<std::array<unsigned, 5>, 2> classMasks{{
    {Mask({Warrior, Hunter, Mage, Rogue, Priest, Warlock, Paladin}),
     Mask({Warrior, Hunter, Rogue, Priest, Paladin, Shaman}),
     Mask({Warrior, Hunter, Rogue, Priest, Druid}),
     Mask({Warrior, Mage, Rogue, Priest, Warlock}),
     Mask({Warrior, Hunter, Mage, Rogue, Druid})},
    {Mask({Warrior, Hunter, Mage, Rogue, Warlock, Shaman}),
     Mask({Warrior, Mage, Rogue, Priest, Warlock, Paladin}),
     Mask({Warrior, Hunter, Druid, Shaman}),
     Mask({Warrior, Hunter, Mage, Rogue, Priest, Warlock, Shaman}),
     Mask({Warrior, Hunter, Rogue, Druid, Shaman})},
}};

struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct StoredProfile {
    DWORD version;
    SessionProfile profile;
    DWORD muted;
};

struct Data {
    DATA_BLOB blob{};
    ~Data() {
        if (blob.pbData) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
};

bool IsFile(const fs::path& file) {
    std::error_code error;
    return fs::is_regular_file(file, error);
}

bool IsLauncher(const std::wstring& path, const wchar_t* file) {
    const fs::path candidate(path);
    return candidate.is_absolute() && _wcsicmp(candidate.filename().c_str(), file) == 0 && IsFile(candidate);
}

fs::path ProfilePath() {
    wchar_t* raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw UserError{L"Windows could not find your local settings folder."};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    return fs::path(folder.get()) / L"WoW Nested" / L"profile.dat";
}

template<std::size_t Length>
std::wstring ReadField(const wchar_t (&field)[Length]) {
    const auto end = std::find(std::begin(field), std::end(field), L'\0');
    if (end == std::end(field))
        throw UserError{L"The saved setup is invalid. Enter your character details again."};
    return {field, end};
}

bool ValidName(const std::wstring& name) {
    if (name.size() < 2 || name.size() > maxNameLength)
        return false;
    std::array<WORD, maxNameLength> types{};
    if (!GetStringTypeW(CT_CTYPE1, name.data(), static_cast<int>(name.size()), types.data()))
        return false;
    return std::all_of(types.begin(), types.begin() + name.size(), [](WORD type) { return (type & C1_ALPHA) != 0; });
}

std::wstring RegistryInstallDirectory(const wchar_t* key) {
    for (const auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (const DWORD view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
            wchar_t path[1024]{};
            DWORD bytes = sizeof path;
            if (RegGetValueW(root, key, L"InstallPath", RRF_RT_REG_SZ | view, nullptr, path, &bytes) == ERROR_SUCCESS)
                return path;
        }
    }
    return {};
}

} // namespace

bool CanPlay(Faction faction, int race, CharacterClass characterClass) {
    const auto side = static_cast<unsigned>(faction);
    const auto slot = static_cast<unsigned>(race);
    const auto kind = static_cast<unsigned>(characterClass);
    return side < factions.size() && slot < races[0].size() && kind < classes.size()
        && (classMasks[side][slot] & (1u << kind)) != 0;
}

std::wstring CharacterSummary(const Profile& profile) {
    return std::wstring(factions[int(profile.faction)]) + L" " + races[int(profile.faction)][profile.race]
        + L" " + classes[int(profile.characterClass)] + L" / " + bodies[int(profile.body)];
}

std::wstring ValidateCharacter(const Profile& profile) {
    if (static_cast<unsigned>(profile.ruleset) >= rulesets.size()
        || static_cast<unsigned>(profile.body) >= bodies.size()
        || !CanPlay(profile.faction, profile.race, profile.characterClass))
        return L"Choose a valid ruleset, faction, race, class, and body type.";
    if (!ValidName(profile.firstName) || !ValidName(profile.secondName))
        return L"Enter both parts of your character name using 2 to 12 letters each.";
    return {};
}

std::wstring ValidateProfile(const Profile& profile) {
    if (auto error = ValidateCharacter(profile); !error.empty())
        return error;
    if (profile.battleNetLauncher.size() >= 1024 || profile.gameLauncher.size() >= 1024)
        return L"Choose launcher paths shorter than 1,024 characters.";
    if (!IsLauncher(profile.battleNetLauncher, L"Battle.net Launcher.exe"))
        return L"Select your Battle.net Launcher.exe.";
    if (!IsLauncher(profile.gameLauncher, L"World of Warcraft Launcher.exe")
        || !HasForeverInstallation(profile.gameLauncher))
        return L"Select the launcher in your WoW: Forever installation folder.";
    return {};
}

bool HasForeverInstallation(const std::wstring& launcher) {
    return IsFile(fs::path(launcher).parent_path() / L"_forever_" / L"Wow.exe");
}

void FindInstallations(Profile& profile) {
    wchar_t directory[1024]{};
    GetEnvironmentVariableW(L"ProgramFiles(x86)", directory, ARRAYSIZE(directory));
    const fs::path common(directory);
    const fs::path battleNetDirectory = RegistryInstallDirectory(L"Software\\Blizzard Entertainment\\Battle.net");
    for (const auto& folder : {battleNetDirectory, common / L"Battle.net"}) {
        const auto path = folder / L"Battle.net Launcher.exe";
        if (IsLauncher(path.wstring(), L"Battle.net Launcher.exe")) {
            profile.battleNetLauncher = path.wstring();
            break;
        }
    }
    const fs::path wowDirectory = RegistryInstallDirectory(L"Software\\Blizzard Entertainment\\World of Warcraft");
    for (const auto& folder : {wowDirectory, common / L"World of Warcraft",
                              common / L"WowForever" / L"World of Warcraft"}) {
        const auto path = folder / L"World of Warcraft Launcher.exe";
        if (IsLauncher(path.wstring(), L"World of Warcraft Launcher.exe") && HasForeverInstallation(path.wstring())) {
            profile.gameLauncher = path.wstring();
            break;
        }
    }
}

SessionProfile ToSessionProfile(const Profile& profile) {
    SessionProfile data{};
    data.ruleset = int(profile.ruleset);
    data.faction = int(profile.faction);
    data.race = profile.race;
    data.body = int(profile.body);
    data.characterClass = int(profile.characterClass);
    wcscpy_s(data.firstName, profile.firstName.c_str());
    wcscpy_s(data.secondName, profile.secondName.c_str());
    wcscpy_s(data.battleNetLauncher, profile.battleNetLauncher.c_str());
    wcscpy_s(data.gameLauncher, profile.gameLauncher.c_str());
    return data;
}

Profile FromSessionProfile(const SessionProfile& data) {
    Profile profile;
    profile.ruleset = static_cast<Ruleset>(data.ruleset);
    profile.faction = static_cast<Faction>(data.faction);
    profile.race = data.race;
    profile.body = static_cast<Body>(data.body);
    profile.characterClass = static_cast<CharacterClass>(data.characterClass);
    profile.firstName = ReadField(data.firstName);
    profile.secondName = ReadField(data.secondName);
    profile.battleNetLauncher = ReadField(data.battleNetLauncher);
    profile.gameLauncher = ReadField(data.gameLauncher);
    return profile;
}

bool LoadProfile(Profile& profile) {
    const auto path = ProfilePath();
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        return false;
    std::array<BYTE, 16384> encrypted{};
    DWORD count = 0;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0
        || size.QuadPart > static_cast<LONGLONG>(encrypted.size())
        || !ReadFile(file.value, encrypted.data(), static_cast<DWORD>(size.QuadPart), &count, nullptr)
        || count != size.QuadPart)
        return false;
    DATA_BLOB input{count, encrypted.data()};
    Data output;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)
        || output.blob.cbData != sizeof(StoredProfile))
        return false;
    const auto* stored = reinterpret_cast<const StoredProfile*>(output.blob.pbData);
    if (stored->version != 1)
        return false;
    try {
        auto candidate = FromSessionProfile(stored->profile);
        if (!ValidateCharacter(candidate).empty())
            return false;
        candidate.muted = stored->muted != 0;
        profile = std::move(candidate);
        return true;
    } catch (const UserError&) {
        return false;
    }
}

void SaveProfile(const Profile& profile) {
    const auto path = ProfilePath();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error)
        throw UserError{L"Could not create the local settings folder."};
    StoredProfile record{1, ToSessionProfile(profile), profile.muted ? 1u : 0u};
    DATA_BLOB input{sizeof record, reinterpret_cast<BYTE*>(&record)};
    Data encrypted;
    const bool protectedData = CryptProtectData(&input, L"WoW Nested character setup", nullptr, nullptr,
        nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob);
    SecureZeroMemory(&record, sizeof record);
    if (!protectedData)
        throw UserError{L"Windows could not encrypt your local setup."};

    const auto temporary = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    bool written = false;
    {
        Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr)};
        DWORD count = 0;
        written = file.value != INVALID_HANDLE_VALUE
            && WriteFile(file.value, encrypted.blob.pbData, encrypted.blob.cbData, &count, nullptr)
            && count == encrypted.blob.cbData && FlushFileBuffers(file.value);
    }
    if (!written || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw UserError{L"Could not save the encrypted setup on this PC."};
    }
}
