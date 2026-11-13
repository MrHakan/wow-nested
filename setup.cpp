#include "setup.h"
#include "resource.h"
#include <commdlg.h>

namespace {

int Selection(HWND dialog, int control) {
    return static_cast<int>(SendDlgItemMessageW(dialog, control, CB_GETCURSEL, 0, 0));
}

template<typename Choices>
void SetChoices(HWND dialog, int control, const Choices& choices, int selected) {
    SendDlgItemMessageW(dialog, control, CB_RESETCONTENT, 0, 0);
    for (const auto* choice : choices)
        SendDlgItemMessageW(dialog, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    SendDlgItemMessageW(dialog, control, CB_SETCURSEL, selected, 0);
}

void SetClasses(HWND dialog, Faction faction, int race, CharacterClass preferred) {
    SendDlgItemMessageW(dialog, IDC_CLASS, CB_RESETCONTENT, 0, 0);
    WPARAM selected = 0;
    for (int index = 0; index < static_cast<int>(classes.size()); ++index) {
        if (!CanPlay(faction, race, static_cast<CharacterClass>(index)))
            continue;
        const auto row = SendDlgItemMessageW(dialog, IDC_CLASS, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(classes[index]));
        SendDlgItemMessageW(dialog, IDC_CLASS, CB_SETITEMDATA, row, index);
        if (index == static_cast<int>(preferred))
            selected = row;
    }
    SendDlgItemMessageW(dialog, IDC_CLASS, CB_SETCURSEL, selected, 0);
}

CharacterClass SelectedClass(HWND dialog) {
    return static_cast<CharacterClass>(SendDlgItemMessageW(dialog, IDC_CLASS, CB_GETITEMDATA,
        Selection(dialog, IDC_CLASS), 0));
}

std::wstring ReadName(HWND dialog, int control) {
    wchar_t name[maxNameLength + 1]{};
    GetDlgItemTextW(dialog, control, name, ARRAYSIZE(name));
    return name;
}

std::wstring PickLauncher(HWND dialog, bool battleNet) {
    wchar_t name[1024]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof picker;
    picker.hwndOwner = dialog;
    picker.lpstrFilter = battleNet ? L"Battle.net Launcher\0Battle.net Launcher.exe\0\0"
                                   : L"World of Warcraft Launcher\0World of Warcraft Launcher.exe\0\0";
    picker.lpstrFile = name;
    picker.nMaxFile = ARRAYSIZE(name);
    picker.lpstrTitle = battleNet ? L"Select Battle.net Launcher.exe"
                                  : L"Select World of Warcraft Launcher.exe";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&picker) ? name : std::wstring{};
}

struct Setup {
    Profile value;
    bool editing;
    HFONT heading = nullptr;

    ~Setup() {
        if (heading)
            DeleteObject(heading);
    }

    void installationStatus(HWND dialog) const {
        SetDlgItemTextW(dialog, IDC_BATTLENET_STATE, value.battleNetLauncher.empty() ? L"Choose launcher" : L"Launcher selected");
        SetDlgItemTextW(dialog, IDC_WOW_STATE, value.gameLauncher.empty() || !HasForeverInstallation(value.gameLauncher)
                         ? L"Choose Forever launcher" : L"Forever installed");
    }

    void initialize(HWND dialog) {
        if (editing) {
            SetWindowTextW(dialog, L"WoW Nested settings");
            SetDlgItemTextW(dialog, IDC_SETUP_HEADING, L"Settings");
        }
        LOGFONTW font{};
        auto base = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
        if (GetObjectW(base, sizeof font, &font)) {
            font.lfHeight *= 2;
            font.lfWeight = FW_BOLD;
            heading = CreateFontIndirectW(&font);
            if (heading)
                SendDlgItemMessageW(dialog, IDC_SETUP_HEADING, WM_SETFONT, reinterpret_cast<WPARAM>(heading), FALSE);
        }
        SetChoices(dialog, IDC_RULESET, rulesets, int(value.ruleset));
        SetChoices(dialog, IDC_FACTION, factions, int(value.faction));
        SetChoices(dialog, IDC_RACE, races[int(value.faction)], value.race);
        SetChoices(dialog, IDC_BODY, bodies, int(value.body));
        SetChoices(dialog, IDC_RETRY_MODE, retryModes, int(value.retryMode));
        describeMode(dialog);
        SetClasses(dialog, value.faction, value.race, value.characterClass);
        SendDlgItemMessageW(dialog, IDC_FIRST_NAME, EM_SETLIMITTEXT, maxNameLength, 0);
        SendDlgItemMessageW(dialog, IDC_SECOND_NAME, EM_SETLIMITTEXT, maxNameLength, 0);
        SetDlgItemTextW(dialog, IDC_FIRST_NAME, value.firstName.c_str());
        SetDlgItemTextW(dialog, IDC_SECOND_NAME, value.secondName.c_str());
        installationStatus(dialog);
    }

    void describeMode(HWND dialog) const {
        const bool loop = Selection(dialog, IDC_RETRY_MODE) == int(RetryMode::Loop);
        SetDlgItemTextW(dialog, IDC_MODE_DESCRIPTION, loop
            ? L"Retries Finish on the same screen after dismissing the population warning."
            : L"Reopens Battle.net and WoW after the population warning.");
    }

    bool save(HWND dialog) {
        value.ruleset = static_cast<Ruleset>(Selection(dialog, IDC_RULESET));
        value.faction = static_cast<Faction>(Selection(dialog, IDC_FACTION));
        value.race = Selection(dialog, IDC_RACE);
        value.body = static_cast<Body>(Selection(dialog, IDC_BODY));
        value.characterClass = SelectedClass(dialog);
        value.retryMode = static_cast<RetryMode>(Selection(dialog, IDC_RETRY_MODE));
        value.firstName = ReadName(dialog, IDC_FIRST_NAME);
        value.secondName = ReadName(dialog, IDC_SECOND_NAME);
        if (const auto error = ValidateProfile(value); !error.empty()) {
            SetDlgItemTextW(dialog, IDC_SETUP_ERROR, error.c_str());
            return false;
        }
        try {
            SaveProfile(value);
        } catch (const UserError& error) {
            SetDlgItemTextW(dialog, IDC_SETUP_ERROR, error.message.c_str());
            return false;
        }
        return true;
    }
};

INT_PTR CALLBACK SetupDialog(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        auto* setup = reinterpret_cast<Setup*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        setup->initialize(dialog);
        SetFocus(GetDlgItem(dialog, IDC_FIRST_NAME));
        return FALSE;
    }
    if (message == WM_CTLCOLORSTATIC && GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_SETUP_ERROR) {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(158, 52, 35));
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (message != WM_COMMAND)
        return FALSE;
    auto& setup = *reinterpret_cast<Setup*>(GetWindowLongPtrW(dialog, DWLP_USER));
    const int control = LOWORD(wParam);
    if (control == IDOK) {
        if (setup.save(dialog))
            EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (control == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (control == IDC_RETRY_MODE && HIWORD(wParam) == CBN_SELCHANGE) {
        setup.describeMode(dialog);
        return TRUE;
    }
    if (HIWORD(wParam) == CBN_SELCHANGE && (control == IDC_FACTION || control == IDC_RACE)) {
        const auto faction = static_cast<Faction>(Selection(dialog, IDC_FACTION));
        const auto preferred = SelectedClass(dialog);
        if (control == IDC_FACTION)
            SetChoices(dialog, IDC_RACE, races[int(faction)], 0);
        SetClasses(dialog, faction, Selection(dialog, IDC_RACE), preferred);
        return TRUE;
    }
    if (control == IDC_BROWSE_BATTLENET || control == IDC_BROWSE_WOW) {
        const bool battleNet = control == IDC_BROWSE_BATTLENET;
        if (auto path = PickLauncher(dialog, battleNet); !path.empty()) {
            (battleNet ? setup.value.battleNetLauncher : setup.value.gameLauncher) = std::move(path);
            setup.installationStatus(dialog);
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool ShowSetup(HINSTANCE instance, HWND parent, Profile& profile) {
    Setup dialog{profile, parent != nullptr};
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETUP), parent, SetupDialog,
                        reinterpret_cast<LPARAM>(&dialog)) != IDOK)
        return false;
    profile = std::move(dialog.value);
    return true;
}
