#include "nested.h"
#include "audio.h"
#include "workflow.h"
#include <tlhelp32.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct Desktop {
    HDESK value = nullptr;
    ~Desktop() { if (value) CloseDesktop(value); }
};

struct Mapping {
    NestedDisplay* view = nullptr;
    ~Mapping() { if (view) UnmapViewOfFile(view); }
};

std::wstring ObjectName(const std::wstring& key, const wchar_t* suffix) {
    return L"Local\\" + key + suffix;
}

LONG Sequence(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

void PublishStatus(NestedDisplay& display, const std::wstring& text) {
    InterlockedIncrement(&display.statusSequence);
    wcsncpy_s(display.status, text.c_str(), _TRUNCATE);
    InterlockedIncrement(&display.statusSequence);
}

void PublishFrame(PanelFrame& panel, const Frame* frame) {
    if (!frame && !panel.visible)
        return;
    InterlockedIncrement(&panel.sequence);
    panel.visible = FALSE;
    if (frame && !frame->pixels.empty()) {
        const double scale = std::min(double(panelWidth) / frame->width, double(panelHeight) / frame->height);
        panel.width = std::max(1, static_cast<int>(frame->width * scale));
        panel.height = std::max(1, static_cast<int>(frame->height * scale));
        for (int y = 0; y < panel.height; ++y) {
            const int sourceY = y * frame->height / panel.height;
            for (int x = 0; x < panel.width; ++x) {
                const int sourceX = x * frame->width / panel.width;
                const auto* source = frame->pixels.data() + (sourceY * frame->width + sourceX) * 4;
                std::memcpy(panel.pixels + (y * panel.width + x) * 4, source, 4);
            }
        }
        panel.visible = TRUE;
    }
    InterlockedIncrement(&panel.sequence);
}

void ConfigureJob(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        const DWORD error = GetLastError();
        throw UserError{L"Could not isolate the apps on a separate desktop. Windows error " + std::to_wstring(error) + L"."};
    }
}

} // namespace

bool BlizzardRunning() {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE)
        throw UserError{L"Could not check for running Blizzard apps."};
    PROCESSENTRY32W process{};
    process.dwSize = sizeof process;
    if (!Process32FirstW(snapshot.value, &process))
        throw UserError{L"Could not check for running Blizzard apps."};
    do {
        for (const wchar_t* name : {L"Wow.exe", L"Battle.net.exe", L"Battle.net Launcher.exe", L"Agent.exe"}) {
            if (_wcsicmp(process.szExeFile, name) == 0)
                return true;
        }
    } while (Process32NextW(snapshot.value, &process));
    return false;
}

std::wstring CreateDesktopName() {
    return L"WoWNested_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
}

class NestedSession::State {
public:
    State(const Profile& profile, bool checkOnly) {
        if (const auto error = ValidateProfile(profile); !error.empty())
            throw UserError{error};
        if (BlizzardRunning())
            throw UserError{L"Quit WoW and Battle.net on your main desktop first."};

        std::wstring name = CreateDesktopName();
        desktop.value = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        stopEvent.value = CreateEventW(nullptr, TRUE, FALSE, ObjectName(name, L"_Stop").c_str());
        memory.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(NestedDisplay), ObjectName(name, L"_Display").c_str());
        const auto jobName = ObjectName(name, L"_Job");
        job.value = CreateJobObjectW(nullptr, jobName.c_str());
        if (!desktop.value || !stopEvent.value || !memory.value || !job.value)
            throw UserError{L"Windows could not create the hidden desktop or its preview."};
        ConfigureJob(job.value);
        mapped.view = static_cast<NestedDisplay*>(MapViewOfFile(memory.value, FILE_MAP_WRITE, 0, 0, sizeof(NestedDisplay)));
        if (!mapped.view)
            throw UserError{L"Could not allocate memory for the previews."};
        mapped.view->profile = ToSessionProfile(profile);
        PublishStatus(*mapped.view, L"Opening Battle.net on the hidden desktop.");

        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size())
            throw UserError{L"Could not find this app's file path."};
        executable.resize(length);
        std::wstring command = L"\"" + executable + L"\" --nested-worker " + name + (checkOnly ? L" check" : L" run");
        std::wstring destination = name;
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.lpDesktop = destination.data();
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                            nullptr, nullptr, &startup, &child))
            throw UserError{L"Could not start the hidden desktop worker."};
        process.value = child.hProcess;
        Handle thread{child.hThread};
        try {
            audio = std::make_unique<AppAudio>(job.value, profile.muted);
        } catch (...) {
            TerminateProcess(process.value, 1);
            throw UserError{L"Could not start the audio controls."};
        }
        if (ResumeThread(thread.value) == DWORD(-1)) {
            TerminateProcess(process.value, 1);
            throw UserError{L"Could not run the hidden desktop worker."};
        }
    }

    Desktop desktop;
    Handle stopEvent, memory, job, process;
    Mapping mapped;
    std::unique_ptr<AppAudio> audio;
};

NestedSession::NestedSession(const Profile& profile, bool checkOnly)
    : state(std::make_unique<State>(profile, checkOnly)) {}
NestedSession::~NestedSession() = default;

void NestedSession::stop() {
    SetEvent(state->stopEvent.value);
}

void NestedSession::setMuted(bool muted) {
    state->audio->setMuted(muted);
}

bool NestedSession::audioFailed() const {
    return state->audio->failed();
}

bool NestedSession::ended() const {
    return WaitForSingleObject(state->process.value, 0) == WAIT_OBJECT_0;
}

DWORD NestedSession::exitCode() const {
    DWORD code = 0;
    GetExitCodeProcess(state->process.value, &code);
    return code;
}

std::wstring NestedSession::status() const {
    auto& display = *state->mapped.view;
    const LONG before = Sequence(&display.statusSequence);
    if (before & 1)
        return {};
    wchar_t text[statusLength]{};
    std::memcpy(text, display.status, sizeof text);
    if (Sequence(&display.statusSequence) != before)
        return {};
    text[statusLength - 1] = L'\0';
    return text;
}

bool NestedSession::readPanel(Frame& frame, LONG& lastSequence) const {
    auto& panel = state->mapped.view->game;
    const LONG before = Sequence(&panel.sequence);
    if (before == lastSequence || (before & 1))
        return false;
    Frame next;
    if (panel.visible && panel.width > 0 && panel.width <= panelWidth && panel.height > 0 && panel.height <= panelHeight) {
        next.width = panel.width;
        next.height = panel.height;
        next.pixels.resize(static_cast<std::size_t>(next.width) * next.height * 4);
        std::memcpy(next.pixels.data(), panel.pixels, next.pixels.size());
    }
    if (Sequence(&panel.sequence) != before)
        return false;
    lastSequence = before;
    frame = std::move(next);
    return true;
}

int RunNestedWorker(const std::wstring& key, bool checkOnly) {
    wchar_t desktop[256]{};
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, nullptr)
        || key != desktop || !key.starts_with(L"WoWNested_"))
        return 2;

    Handle stopEvent{OpenEventW(SYNCHRONIZE, FALSE, ObjectName(key, L"_Stop").c_str())};
    Handle memory{OpenFileMappingW(FILE_MAP_WRITE, FALSE, ObjectName(key, L"_Display").c_str())};
    Handle job{OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS, FALSE, ObjectName(key, L"_Job").c_str())};
    if (!stopEvent.value || !memory.value || !job.value)
        return 2;
    Mapping mapped{static_cast<NestedDisplay*>(MapViewOfFile(memory.value, FILE_MAP_WRITE, 0, 0, sizeof(NestedDisplay)))};
    if (!mapped.view)
        return 2;
    std::stop_source cancellation;
    std::jthread watch([&](std::stop_token done) {
        while (!done.stop_requested()) {
            if (WaitForSingleObject(stopEvent.value, 100) != WAIT_TIMEOUT) {
                cancellation.request_stop();
                return;
            }
        }
    });

    const auto report = [&](const std::wstring& text) { PublishStatus(*mapped.view, text); };
    const NestedFrames frames{[&](const Frame* frame) {
        PublishFrame(mapped.view->game, frame);
    }};
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            const auto profile = FromSessionProfile(mapped.view->profile);
            SecureZeroMemory(&mapped.view->profile, sizeof mapped.view->profile);
            if (const auto error = ValidateProfile(profile); !error.empty())
                throw UserError{error};
            RunNestedWorkflow(cancellation.get_token(), report, frames, profile, checkOnly, job.value);
        } catch (...) {
            winrt::uninit_apartment();
            throw;
        }
        winrt::uninit_apartment();
    } catch (const Cancelled&) {
        report(L"Stopped. The hidden desktop windows were closed.");
    } catch (const UserError& error) {
        report(error.message);
    } catch (const winrt::hresult_error&) {
        report(L"Windows could not read the game window.");
    } catch (...) {
        report(L"Stopped after an unexpected error.");
    }
    return 0;
}
