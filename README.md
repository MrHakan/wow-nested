# WoW Nested

WoW Nested is a portable Windows companion for the World of Warcraft: Forever character creator. It runs Battle.net and WoW on a separate Windows desktop and shows progress in a small window on your main desktop. It can start from the initial ruleset picker or from an existing character list.

## Getting started

1. Install WoW: Forever and sign in through Battle.net. Quit both apps before starting WoW Nested.
2. Open the executable and complete the welcome screen. Choose the ruleset, faction, race, class, body type, both parts of the character name, and the retry mode. If an installation needs to be located, select its launcher with **Browse**.
3. Use **Test setup** to check the selections and name entry. The test ends before submission.
4. Choose a retry mode and press **Start**. **Loop mode** stays on the Customize screen and retries **Finish** one second after dismissing a confirmed population imbalance warning. **Battle.net Restart mode** reopens WoW and Battle.net after that warning. In both modes, the session reopens the apps if the game closes, the login servers are full, or an attempt fails. Consecutive failures increase the retry pause from 10 seconds up to one minute. The session ends when WoW returns to the character list after submission. Open WoW normally to confirm the result.

**Stop** closes the session. **Mute audio** controls the apps launched by the session.

WoW Nested waits up to 20 seconds for name validation and reports when WoW rejects the name. To change a rejected name, press **Stop** and open **Settings**.

Use **Settings** to change your choices between runs. The retry mode is also available in the main window. This version expects the English WoW: Forever character creation screens at 2560 × 1440 or 1920 × 1080. The resolution is detected from the WoW window. The starting screen and allowed characters can depend on your current game state and entitlements.

## Privacy

Setup is encrypted with Windows Data Protection and stored in the current user's local application data. The setup screen shows whether the launchers were found and keeps their full paths private. The dashboard previews the login queue, ruleset picker, character creation, and Customize screens while it is in the foreground. The preview covers the name fields and pauses when the dashboard loses focus or is minimized. It excludes Battle.net, credential entry, and saved character lists.

WoW Nested contains no telemetry, networking code, account sign-in form, or screen recording. It uses the Battle.net session you have already opened through Blizzard's launcher. Battle.net and WoW make their own network connections.

## Build from source

Use an **x64 Native Tools Command Prompt for VS 2022** with the MSVC C++ toolset and a Windows 11 SDK containing the C++/WinRT headers. Run `build.cmd` from the repository root. The executable is written to `build\WoW-Nested.exe`.

## License

See [LICENSE](LICENSE) for the GPL-3.0 terms covering this project's source code. Small in-game interface fragments in `assets/` are used as visual references. World of Warcraft and Battle.net are Blizzard Entertainment trademarks. WoW Nested is an independent project. Review the [Blizzard End User License Agreement](https://www.blizzard.com/en-us/legal/08b946df-660a-40e4-a072-1fbde65173b1/blizzard-end-user-license-agreement) before using game automation.
