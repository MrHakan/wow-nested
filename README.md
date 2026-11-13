# WoW Nested

WoW Nested is a portable Windows companion for the World of Warcraft: Forever character creator. It runs Battle.net and WoW on a separate Windows desktop and shows progress in a small window on your main desktop.

## Getting started

1. Install WoW: Forever and sign in through Battle.net. Quit both apps before starting WoW Nested.
2. Open the executable and complete the welcome screen. Choose the ruleset, faction, race, class, body type, and both parts of the character name. If an installation needs to be located, select its launcher with **Browse**.
3. Use **Test setup** to check the selections and name entry. The test ends before submission.
4. Use **Start loop** for a live attempt. WoW Nested retries only when it confirms the population imbalance warning. The selected ruleset and faction are used on every attempt. **Stop** closes the session. **Mute audio** controls the apps launched by the session. After the loop stops on a different result, close the session and open WoW normally to check the character list.

Use **Setup** to change your choices between runs. This version expects the English WoW: Forever character creation screens at 2560 × 1440. The starting screen and allowed characters can depend on your current game state and entitlements. WoW Nested stops when it cannot verify a selection or encounters an unexpected screen.

## Privacy

Setup is encrypted with Windows Data Protection and stored in the current user's local application data. The setup screen shows whether the launchers were found and keeps their full paths private. The dashboard shows session status and the supported character creation screens. It does not display Battle.net, sign-in screens, character lists, or the name-entry screen.

WoW Nested contains no telemetry, networking code, account sign-in form, or screen recording. It uses the Battle.net session you have already opened through Blizzard's launcher. Battle.net and WoW make their own network connections.

## Build from source

Use an **x64 Native Tools Command Prompt for VS 2022** with the MSVC C++ toolset and a Windows 11 SDK containing the C++/WinRT headers. Run `build.cmd` from the repository root. The executable is written to `build\WoW-Nested.exe`.

## License

See [LICENSE](LICENSE) for the GPL-3.0 terms covering this project's source code. Small in-game interface fragments in `assets/` are used as visual references. World of Warcraft and Battle.net are Blizzard Entertainment trademarks. WoW Nested is an independent project. Review the [Blizzard End User License Agreement](https://www.blizzard.com/en-us/legal/08b946df-660a-40e4-a072-1fbde65173b1/blizzard-end-user-license-agreement) before using game automation.
