#include "workflow.h"
#include <array>
#include <optional>

using Clock = std::chrono::steady_clock;

namespace {

enum class Mark {
    Select, FullName, Population, Okay, FirstEmpty, LastEmpty, NameValid, RulesTitle, AllianceLabel, HordeLabel,
    CharacterListRuleset, CreateCharacter, LoginQueue, LoginUnavailable, NameRejected
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
    Pattern{122, {1380, 15, 1500, 59}, 42, true},
    Pattern{123, {2135, 1282, 2340, 1329}, 42, true},
    Pattern{124, {1100, 626, 1460, 691}, 42, true},
    Pattern{125, {1130, 678, 1430, 740}, 42, true},
    Pattern{126, {1670, 45, 1740, 89}, 45, true},
};

constexpr POINT finishButton{2355, 1366};

bool FinishButtonEnabled(const Frame& frame) {
    // The enabled button has red fill on both sides of its label.
    for (const int side : {2240, 2440}) {
        int red = 0;
        for (int y = 1354; y <= 1384; y += 5) {
            for (int x = side; x < side + 30; x += 5) {
                const auto* pixel = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4;
                if (pixel[2] >= 65 && pixel[2] > pixel[1] + 40 && pixel[2] > pixel[0] + 40)
                    ++red;
            }
        }
        if (red < 30)
            return false;
    }
    return true;
}

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
        return untilDeadline(Clock::now() + timeout, predicate);
    }

    template<typename Predicate>
    bool untilDeadline(const Clock::time_point& deadline, Predicate predicate) {
        do {
            CheckStop(stop);
            waitForResponse();
            const Frame* frame = readFrame();
            if (frame) {
                if (frame->width != 2560 || frame->height != 1440)
                    throw UserError{L"Set WoW to 2560 × 1440 in its display options and try again."};
                if (frames.enabled()) {
                    const bool customize = images.has(*frame, Mark::FullName);
                    const bool supported = customize || images.has(*frame, Mark::RulesTitle) || characterScreen(*frame)
                        || images.has(*frame, Mark::LoginQueue);
                    frames.publish(supported ? frame : nullptr, customize);
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
    void movePointer(POINT point) {
        CheckStop(stop);
        MovePointerInWindow(window, point, false);
    }

    bool characterScreen(const Frame& frame) const {
        return images.has(frame, Mark::AllianceLabel) && images.has(frame, Mark::HordeLabel);
    }

    bool characterListReady(const Frame& frame) const {
        // The Ruleset tab is drawn while the character list is still loading.
        return images.has(frame, Mark::CharacterListRuleset) && images.has(frame, Mark::CreateCharacter);
    }

    void openRulesetScreen(const Report& report, bool restartOnQueue) {
        bool characterList = false;
        bool queued = false;
        const auto started = Clock::now();
        const auto queueDeadline = started + 30min;
        auto deadline = started + 120s;
        std::optional<Clock::time_point> listReadySince;
        const bool found = untilDeadline(deadline, [&](const Frame& frame) {
            if (images.has(frame, Mark::LoginUnavailable))
                throw UserError{L"WoW reports that no servers are available (BLZ51934200)."};
            if (images.has(frame, Mark::RulesTitle)) {
                characterList = false;
                return true;
            }
            const auto now = Clock::now();
            const bool inQueue = images.has(frame, Mark::LoginQueue);
            if (inQueue && restartOnQueue)
                throw UserError{L"WoW reports that the login servers are full."};
            if (inQueue != queued) {
                queued = inQueue;
                deadline = queued ? queueDeadline : now + 120s;
                report(queued ? L"WoW is in the login queue. Waiting for a server slot."
                              : L"WoW left the login queue. Waiting for the character screen.");
            }
            characterList = characterListReady(frame);
            if (!characterList) {
                listReadySince.reset();
                return false;
            }
            if (!listReadySince)
                listReadySince = now;
            return now - *listReadySince >= 2s;
        });
        if (!found) {
            if (queued)
                throw UserError{L"WoW is still in the login queue after 30 minutes."};
            throw UserError{L"WoW did not reach the ruleset picker or character list. Check the game connection."};
        }
        if (characterList) {
            report(L"WoW opened the character list. Opening the ruleset picker.");
            click({1437, 38});
            if (has(Mark::RulesTitle, 12s))
                return;
            if (until(0ms, [&](const Frame& frame) { return characterListReady(frame); }))
                click({1437, 38});
            require(Mark::RulesTitle, 45s, L"WoW did not return the ruleset choices.");
        }
    }

    bool populationError(std::chrono::milliseconds timeout) {
        std::optional<Clock::time_point> firstSeen;
        bool returnedToList = false;
        const bool response = until(timeout, [&](const Frame& frame) {
            if (images.has(frame, Mark::LoginUnavailable))
                throw UserError{L"WoW reports that no servers are available (BLZ51934200)."};
            returnedToList = characterListReady(frame) && !images.has(frame, Mark::FullName);
            if (returnedToList)
                return true;
            if (!images.has(frame, Mark::Population) || !images.has(frame, Mark::Okay)) {
                firstSeen.reset();
                return false;
            }
            const auto now = Clock::now();
            if (!firstSeen)
                firstSeen = now;
            return now - *firstSeen >= 1s;
        });
        if (!response)
            throw UserError{L"WoW did not confirm the result of character creation."};
        return !returnedToList;
    }

    void dismissPopulation(bool useEnter = false) {
        const auto dismissed = [&](const Frame& frame) { return !images.has(frame, Mark::Population); };
        if (useEnter) {
            key(VK_RETURN);
            if (until(1s, dismissed))
                return;
            require(Mark::Okay, 0ms, L"Could not confirm the Okay button on the population imbalance warning.");
        }
        click({1280, 762});
        if (!until(3s, dismissed))
            throw UserError{L"WoW did not dismiss the population imbalance warning."};
    }

    void waitForNameApproval() {
        std::optional<Clock::time_point> rejectionSince;
        if (until(20s, [&](const Frame& frame) {
            if (validName(frame))
                return true;
            const bool rejected = images.has(frame, Mark::FullName)
                && images.has(frame, Mark::NameRejected);
            if (rejected) {
                const auto now = Clock::now();
                if (!rejectionSince)
                    rejectionSince = now;
            } else {
                rejectionSince.reset();
            }
            return false;
        }))
            return;
        if (rejectionSince && Clock::now() - *rejectionSince >= 3s)
            throw UserError{L"WoW rejected the character name. Use Stop and Settings to change it."};
        throw UserError{L"WoW did not confirm the character name within 20 seconds."};
    }

    bool finishReady() {
        return until(5s, [&](const Frame& frame) {
            return validName(frame) && FinishButtonEnabled(frame)
                && !images.has(frame, Mark::Population) && !images.has(frame, Mark::Okay);
        });
    }

private:
    void waitForResponse() const {
        const auto deadline = Clock::now() + 30s;
        do {
            CheckStop(stop);
            if (!IsWindow(window) || !IsWindowVisible(window))
                throw UserError{L"The WoW window closed."};
            if (SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK | SMTO_ERRORONEXIT, 250, nullptr))
                return;
            Pause(stop, 125ms);
        } while (Clock::now() < deadline);
        throw UserError{L"WoW did not respond for 30 seconds."};
    }

    const Frame* readFrame() {
        // PrintWindow may block even after the window answered the readiness check.
        struct CaptureActivity {
            const std::function<void(bool)>& busy;
            explicit CaptureActivity(const std::function<void(bool)>& callback) : busy(callback) { busy(true); }
            ~CaptureActivity() { busy(false); }
        } activity(frames.captureBusy);
        return capture.Latest();
    }

    bool validName(const Frame& frame) const {
        return images.has(frame, Mark::FullName)
            && !images.has(frame, Mark::FirstEmpty)
            && !images.has(frame, Mark::LastEmpty)
            && images.has(frame, Mark::NameValid);
    }

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
        if (!view.until(3s, selected) && view.has(Mark::RulesTitle))
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
    if (!view.until(2s, bodySelected)) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            view.click({bodyX, 56});
            if (view.until(3s, bodySelected))
                break;
        }
        if (!view.until(0ms, bodySelected))
            throw UserError{L"Could not confirm the requested body type."};
    }
    if (!view.until(0ms, raceSelected)) {
        if (!view.until(3s, [=](const Frame& frame) { return EnabledPortrait(frame, {raceX, raceY}); }))
            throw UserError{L"The selected race is unavailable in WoW."};
        view.click({raceX, raceY});
        if (!view.until(3s, raceSelected)) {
            view.click({raceX, raceY});
            if (!view.until(5s, raceSelected))
                throw UserError{L"Could not confirm the requested race."};
        }
    }
    if (!view.until(0ms, classSelected)) {
        view.click({classX, 1345});
        if (!view.until(3s, classSelected)) {
            view.click({classX, 1345});
            if (!view.until(5s, classSelected))
                throw UserError{L"Could not confirm the requested class. It may be unavailable for this race."};
        }
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
    // Move off the indicator so a tooltip cannot interfere with validation.
    view.movePointer({1280, 180});
    report(L"Waiting for WoW to validate the character name.");
    view.waitForNameApproval();
}

void RepeatFinish(View& view, std::stop_token stop, const Report& report) {
    for (unsigned int attempt = 1; ; ++attempt) {
        if (!view.finishReady())
            throw UserError{L"Finish is unavailable. Check the character name and game state."};
        report(L"Clicking Finish. Attempt " + std::to_wstring(attempt) + L".");
        view.click(finishButton);
        if (!view.populationError(20s)) {
            report(L"WoW returned to the character list. Open WoW normally to check the result.");
            return;
        }
        view.dismissPopulation(true);
        report(L"Population imbalance warning dismissed. Retrying Finish in 1 second.");
        Pause(stop, 1s);
    }
}

bool SessionHasApps(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr))
        throw UserError{L"Windows could not check the apps from the previous attempt."};
    return info.ActiveProcesses != 0;
}

void RequestSessionClose(App app, HANDLE job) {
    if (const HWND window = FindApp(app)) {
        try {
            Process process(window);
            BOOL belongs = FALSE;
            if (IsProcessInJob(process.get(), job, &belongs) && belongs)
                RequestWindowClose(window);
        } catch (const UserError&) {
            // The window may already have closed. Job cleanup also handles unresponsive apps.
        }
    }
}

void CloseSessionApps(HANDLE job, std::stop_token stop) {
    if (!SessionHasApps(job))
        return;
    if (FindApp(App::Game)) {
        RequestSessionClose(App::Game, job);
        const auto deadline = Clock::now() + 5s;
        while (FindApp(App::Game) && Clock::now() < deadline)
            Pause(stop, 100ms);
        Pause(stop, 1s);
    }
    RequestSessionClose(App::Launcher, job);
    const auto gracefulDeadline = Clock::now() + 3s;
    while (SessionHasApps(job) && Clock::now() < gracefulDeadline)
        Pause(stop, 100ms);
    CheckStop(stop);
    if (SessionHasApps(job) && !TerminateJobObject(job, 1))
        throw UserError{L"Windows could not close the apps from the previous attempt."};
    const auto forcedDeadline = Clock::now() + 10s;
    while (SessionHasApps(job)) {
        if (Clock::now() >= forcedDeadline)
            throw UserError{L"The apps from the previous attempt are still closing."};
        Pause(stop, 100ms);
    }
}

// Returns true when the current attempt needs a fresh game and launcher.
bool RunAttempt(std::stop_token stop, const Report& report, const NestedFrames& frames,
                const Profile& profile, WorkflowMode mode, HANDLE job, const Images& images) {
    OpenLauncher(job, profile.battleNetLauncher);
    WaitForApp(App::Launcher, 45s, stop);
    report(L"Battle.net is open. Starting WoW: Forever.");
    OpenWoWForever(job, profile.gameLauncher);
    const HWND game = WaitForApp(App::Game, 55s, stop);

    View view(game, images, stop, frames);
    report(L"Waiting for WoW to connect.");
    view.openRulesetScreen(report, mode != WorkflowMode::TestSetup);
    ChooseRuleset(view, profile.ruleset, report);
    report(L"Loading the character screen.");
    bool characterList = false;
    if (!view.until(60s, [&](const Frame& frame) {
        characterList = view.characterListReady(frame);
        return characterList || view.characterScreen(frame);
    }))
        throw UserError{L"The character creation screen did not load in time."};
    if (characterList) {
        report(L"Opening Create Character on the selected ruleset.");
        view.click({2247, 1307});
        if (!view.until(20s, [&](const Frame& frame) { return view.characterScreen(frame); }))
            throw UserError{L"Create Character did not open the character creation screen."};
    }
    SelectCharacter(view, profile, report);
    EnterNames(view, profile, report);
    if (mode == WorkflowMode::TestSetup) {
        report(L"Test passed. Verified the selected ruleset, character, and name entry.");
        return false;
    }
    if (mode == WorkflowMode::FinishLoop) {
        RepeatFinish(view, stop, report);
        return false;
    }
    view.key(VK_RETURN);
    report(L"Checking the result of character creation.");
    if (!view.populationError(20s)) {
        report(L"WoW returned to the character list. Open WoW normally to check the result.");
        return false;
    }
    report(L"Population imbalance warning confirmed.");
    view.dismissPopulation();
    return true;
}

} // namespace

void RunNestedWorkflow(std::stop_token stop, const Report& report, const NestedFrames& frames,
                       const Profile& profile, WorkflowMode mode, HANDLE job) {
    const Images images;
    auto failureDelay = 10s;
    for (unsigned long long attempt = 1; ; ++attempt) {
        CheckStop(stop);
        std::wstring reason;
        try {
            // A failed cleanup must be retried before launching another copy.
            CloseSessionApps(job, stop);
            report(L"Opening Battle.net. Attempt " + std::to_wstring(attempt) + L".");
            if (!RunAttempt(stop, report, frames, profile, mode, job, images))
                return;
            failureDelay = 10s;
        } catch (const UserError& error) {
            if (mode == WorkflowMode::TestSetup)
                throw;
            reason = error.message;
        }
        CheckStop(stop);
        frames.publish(nullptr, false);
        const auto delay = reason.empty() ? 2s : failureDelay;
        if (!reason.empty())
            failureDelay = std::min(failureDelay * 2, 60s);
        const auto retry = L"Retrying after cleanup and a " + std::to_wstring(delay.count()) + L"-second pause.";
        report((reason.empty() ? L"Restarting WoW and Battle.net. " : reason + L" ") + retry);
        try {
            CloseSessionApps(job, stop);
        } catch (const UserError& error) {
            report(error.message + L" " + retry);
        }
        Pause(stop, delay);
    }
}

