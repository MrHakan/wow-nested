#include "workflow.h"
#include <array>
#include <optional>

using Clock = std::chrono::steady_clock;

namespace {

enum class Mark {
    Select, FullName, Population, Okay, FirstEmpty, LastEmpty, NameValid, RulesTitle, AllianceLabel, HordeLabel
};

struct Pattern {
    int resource;
    RECT region;
    int tolerance;
    bool masked = false;
};

constexpr std::array patterns{
    Pattern{104, {2255, 1345, 2355, 1382}, 38},
    Pattern{109, {1140, 5, 1430, 44}, 38},
    Pattern{110, {956, 648, 1610, 736}, 28, true},
    Pattern{111, {1145, 735, 1260, 774}, 38},
    Pattern{116, {955, 46, 1120, 87}, 34},
    Pattern{117, {1330, 46, 1535, 87}, 34},
    Pattern{118, {1670, 45, 1740, 89}, 45, true},
    Pattern{119, {1030, 285, 1530, 345}, 28, true},
    Pattern{120, {35, 96, 171, 133}, 35, true},
    Pattern{121, {270, 96, 392, 133}, 35, true},
};

bool SelectedBorder(const Frame& frame, int centerX, int top, int bottom, int span) {
    // A selected tile has a continuous bright gold line across the top.
    for (int y = top; y <= bottom; ++y) {
        int continuous = 0;
        for (int x = centerX - span / 2; x < centerX + span / 2; ++x) {
            const auto* pixel = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4;
            if (pixel[2] >= 235 && pixel[1] >= 175 && int(pixel[2]) - pixel[0] >= 24) {
                if (++continuous >= span - 4)
                    return true;
            } else {
                continuous = 0;
            }
        }
    }
    return false;
}

bool EnabledPortrait(const Frame& frame, POINT center) {
    int colored = 0;
    for (LONG y = center.y - 26; y <= center.y + 26; y += 2) {
        for (LONG x = center.x - 26; x <= center.x + 26; x += 2) {
            const auto* pixel = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4;
            if (std::max({pixel[0], pixel[1], pixel[2]}) - std::min({pixel[0], pixel[1], pixel[2]}) > 18)
                ++colored;
        }
    }
    return colored > 25;
}

class Images {
public:
    Images() {
        images.reserve(patterns.size());
        for (const auto& pattern : patterns)
            images.emplace_back(pattern.resource, pattern.masked);
    }

    bool has(const Frame& frame, Mark mark) const {
        const auto index = static_cast<std::size_t>(mark);
        const auto& pattern = patterns[index];
        return images[index].Matches(frame, pattern.region, pattern.tolerance);
    }

private:
    std::vector<ImageTemplate> images;
};

class View {
public:
    View(HWND window, const Images& images, std::stop_token stop, const NestedFrames& frames)
        : window(window), images(images), stop(stop), frames(frames), capture(window) {}

    template<typename Predicate>
    bool until(std::chrono::milliseconds timeout, Predicate predicate) {
        const auto deadline = Clock::now() + timeout;
        do {
            CheckStop(stop);
            const Frame* frame = capture.Latest();
            if (frame) {
                if (frame->width != 2560 || frame->height != 1440)
                    throw UserError{L"Set WoW to 2560 × 1440 in its display options and try again."};
                if (images.has(*frame, Mark::RulesTitle) || characterScreen(*frame)) {
                    frames.publish(frame);
                } else {
                    frames.publish(nullptr);
                }
                if (predicate(*frame))
                    return true;
            }
            if (Clock::now() >= deadline)
                return false;
            Pause(stop, 125ms);
        } while (true);
    }

    bool has(Mark mark, std::chrono::milliseconds timeout = 0ms) {
        return until(timeout, [&](const Frame& frame) { return images.has(frame, mark); });
    }

    void require(Mark mark, std::chrono::milliseconds timeout, const std::wstring& error) {
        if (!has(mark, timeout))
            throw UserError{error};
    }

    void click(POINT point) {
        CheckStop(stop);
        ClickInWindow(window, point, false, stop);
    }

    void type(const std::wstring& text) { TypeInWindow(window, text, stop); }
    void key(UINT keyCode) { PressInWindow(window, keyCode, stop); }

    bool characterScreen(const Frame& frame) const {
        return images.has(frame, Mark::AllianceLabel) && images.has(frame, Mark::HordeLabel);
    }

    bool populationError(std::chrono::milliseconds timeout) {
        std::optional<Clock::time_point> firstSeen;
        return until(timeout, [&](const Frame& frame) {
            if (!images.has(frame, Mark::Population) || !images.has(frame, Mark::Okay)) {
                firstSeen.reset();
                return false;
            }
            const auto now = Clock::now();
            if (!firstSeen)
                firstSeen = now;
            return now - *firstSeen >= 1s;
        });
    }

    void dismissPopulation() {
        click({1280, 762});
        if (!until(3s, [&](const Frame& frame) { return !images.has(frame, Mark::Population); }))
            throw UserError{L"WoW did not dismiss the population imbalance warning."};
    }

    bool namesAccepted() {
        return until(5s, [&](const Frame& frame) {
            return images.has(frame, Mark::FullName)
                && !images.has(frame, Mark::FirstEmpty)
                && !images.has(frame, Mark::LastEmpty)
                && images.has(frame, Mark::NameValid);
        });
    }

private:
    HWND window;
    const Images& images;
    std::stop_token stop;
    const NestedFrames& frames;
    HiddenCapture capture;
};

HWND WaitForApp(App app, std::chrono::milliseconds timeout, std::stop_token stop) {
    const auto deadline = Clock::now() + timeout;
    do {
        CheckStop(stop);
        if (HWND window = FindApp(app)) {
            RequireBackground(window);
            return window;
        }
        Pause(stop, 200ms);
    } while (Clock::now() < deadline);
    throw UserError{app == App::Game ? L"Could not find the WoW: Forever window."
                                     : L"Could not find the Battle.net window."};
}

void ChooseRuleset(View& view, Ruleset ruleset, const Report& report) {
    const int center = 768 + 340 * int(ruleset);
    report(L"Selecting " + std::wstring(rulesets[int(ruleset)]) + L".");
    const auto selected = [center](const Frame& frame) { return SelectedBorder(frame, center, 467, 480, 125); };
    if (!view.until(0ms, selected)) {
        view.click({center, 620});
        if (!view.until(5s, selected))
            throw UserError{L"Could not confirm the requested ruleset."};
    }
    view.require(Mark::Select, 5s, L"Could not find the Select button on the ruleset screen.");
    view.click({2461, 1386});
}

void SelectCharacter(View& view, const Profile& profile, const Report& report) {
    report(L"Selecting your character.");
    const bool horde = profile.faction == Faction::Horde;
    const int raceX = horde ? 330 : 104;
    const int raceY = 190 + 116 * profile.race;
    const int bodyX = profile.body == Body::Type1 ? 1234 : 1327;
    const int classX = 820 + 115 * int(profile.characterClass);
    const auto raceSelected = [=](const Frame& frame) {
        return SelectedBorder(frame, raceX, raceY - 57, raceY - 45, 60);
    };
    const auto bodySelected = [=](const Frame& frame) {
        return SelectedBorder(frame, bodyX, 17, 27, 44);
    };
    const auto classSelected = [=](const Frame& frame) {
        return SelectedBorder(frame, classX, 1290, 1301, 50);
    };
    if (!view.until(0ms, raceSelected)) {
        if (!view.until(0ms, [=](const Frame& frame) { return EnabledPortrait(frame, {raceX, raceY}); }))
            throw UserError{L"The selected race is unavailable in WoW."};
        view.click({raceX, raceY});
        if (!view.until(5s, raceSelected))
            throw UserError{L"Could not confirm the requested race."};
    }
    if (!view.until(0ms, bodySelected)) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            view.click({bodyX, 56});
            if (view.until(2s, bodySelected))
                break;
        }
        if (!view.until(0ms, bodySelected))
            throw UserError{L"Could not confirm the requested body type."};
    }
    if (!view.until(0ms, classSelected)) {
        view.click({classX, 1345});
        if (!view.until(5s, classSelected))
            throw UserError{L"Could not confirm the requested class. It may be unavailable for this race."};
    }
    if (!view.until(1s, [&](const Frame& frame) {
        return raceSelected(frame) && bodySelected(frame) && classSelected(frame);
    }))
        throw UserError{L"Could not verify the complete character selection."};
    view.click({2369, 1375});
}

void EnterNames(View& view, const Profile& profile, const Report& report) {
    report(L"Entering the character name.");
    view.require(Mark::FullName, 5s, L"WoW did not open the Customize screen.");
    view.require(Mark::FirstEmpty, 2s, L"Could not verify that the first name field was empty.");
    view.require(Mark::LastEmpty, 2s, L"Could not verify that the second name field was empty.");
    view.click({1100, 78});
    view.type(profile.firstName);
    view.click({1455, 78});
    view.type(profile.secondName);
    if (!view.namesAccepted())
        throw UserError{L"Could not verify the character name. The submission was skipped."};
}

void CloseLauncher(HWND launcher, std::stop_token stop) {
    if (!launcher || !IsWindow(launcher))
        return;
    if (FindApp(App::Launcher) != launcher)
        throw UserError{L"Could no longer verify the Battle.net window."};
    Process process(launcher, PROCESS_TERMINATE);
    RequestWindowClose(launcher);
    try {
        WaitForExit(process.get(), 8s, stop, L"The Battle.net process did not close.");
    } catch (const UserError&) {
        CheckStop(stop);
        if ((IsWindow(launcher) && IsWindowVisible(launcher)) || !ProcessRunning(process.get()))
            throw;
        if (!TerminateProcess(process.get(), 0))
            throw UserError{L"Could not close Battle.net from the system tray."};
        WaitForExit(process.get(), 5s, stop, L"The Battle.net process did not close.");
    }
}

} // namespace

void RunNestedWorkflow(std::stop_token stop, const Report& report, const NestedFrames& frames,
                       const Profile& profile, bool checkOnly, HANDLE job) {
    const Images images;
    for (int attempt = 1; ; ++attempt) {
        CheckStop(stop);
        report(L"Opening Battle.net. Attempt " + std::to_wstring(attempt) + L".");
        OpenLauncher(job, profile.battleNetLauncher);
        const HWND launcher = WaitForApp(App::Launcher, 45s, stop);
        report(L"Battle.net is open. Starting WoW: Forever.");
        OpenWoWForever(job, profile.gameLauncher);
        const HWND game = WaitForApp(App::Game, 55s, stop);

        {
            View view(game, images, stop, frames);
            report(L"Waiting for WoW to connect.");
            view.require(Mark::RulesTitle, 120s, L"The ruleset screen did not load. Check your Battle.net sign-in and game state.");
            ChooseRuleset(view, profile.ruleset, report);
            report(L"Loading the character screen.");
            if (!view.until(60s, [&](const Frame& frame) { return view.characterScreen(frame); }))
                throw UserError{L"The character creation screen did not load in time."};
            SelectCharacter(view, profile, report);
            EnterNames(view, profile, report);
            if (checkOnly) {
                report(L"Test passed. Verified the selected ruleset, character, and name entry.");
                return;
            }
            view.key(VK_RETURN);
            report(L"Checking for the population imbalance warning.");
            if (!view.populationError(20s)) {
                report(L"No population imbalance warning appeared. The loop has stopped. Open WoW normally to check the result.");
                while (IsWindow(game) && IsWindowVisible(game)) {
                    Pause(stop, 300ms);
                    view.until(0ms, [](const Frame&) { return true; });
                }
                return;
            }

            report(L"Population imbalance warning confirmed.");
            view.dismissPopulation();
        }

        Process gameProcess(game);
        RequestWindowClose(game);
        const auto closeDeadline = Clock::now() + 20s;
        while (IsWindow(game) && IsWindowVisible(game)) {
            if (Clock::now() >= closeDeadline)
                throw UserError{L"The WoW window did not close. The loop has stopped."};
            Pause(stop, 100ms);
        }

        Pause(stop, 1s);
        CloseLauncher(launcher, stop);
        frames.publish(nullptr);
        report(L"Retrying in 2 seconds.");
        Pause(stop, 2s);
        if (ProcessRunning(gameProcess.get())) {
            report(L"Waiting for WoW to finish closing.");
            WaitForExit(gameProcess.get(), 25s, stop, L"The WoW process did not close. The loop has stopped.");
        }
    }
}

