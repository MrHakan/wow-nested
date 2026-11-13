#include "audio.h"
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <unordered_map>

namespace {

struct TaskString {
    LPWSTR value = nullptr;
    ~TaskString() { CoTaskMemFree(value); }
};

bool BelongsToJob(DWORD pid, HANDLE job) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    BOOL belongs = FALSE;
    const BOOL checked = IsProcessInJob(process, job, &belongs);
    CloseHandle(process);
    return checked && belongs;
}

struct MutedSession {
    std::wstring group;
    winrt::com_ptr<IAudioSessionControl2> control;
    winrt::com_ptr<ISimpleAudioVolume> volume;
};

class AudioSessions {
public:
    explicit AudioSessions(HANDLE job) : job_(job) {
        winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devices_.put())));
    }

    ~AudioSessions() { restore(); }

    bool apply() {
        winrt::com_ptr<IMMDeviceCollection> endpoints;
        if (FAILED(devices_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.put())))
            return false;
        UINT count = 0;
        if (FAILED(endpoints->GetCount(&count)))
            return false;

        bool success = true;
        for (UINT index = 0; index < count; ++index) {
            winrt::com_ptr<IMMDevice> endpoint;
            if (SUCCEEDED(endpoints->Item(index, endpoint.put())))
                success = applyToEndpoint(endpoint.get()) && success;
        }
        return success;
    }

    bool restore() {
        bool success = true;
        for (auto entry = changed_.begin(); entry != changed_.end();) {
            if (SUCCEEDED(entry->second.volume->SetMute(FALSE, nullptr))) {
                entry = changed_.erase(entry);
                continue;
            }
            AudioSessionState state{};
            if (SUCCEEDED(entry->second.control->GetState(&state)) && state != AudioSessionStateExpired)
                success = false;
            ++entry;
        }
        return success;
    }

private:
    bool applyToEndpoint(IMMDevice* endpoint) {
        winrt::com_ptr<IAudioSessionManager2> manager;
        if (FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void())))
            return false;
        winrt::com_ptr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.put())))
            return false;
        int count = 0;
        if (FAILED(sessions->GetCount(&count)))
            return false;

        bool success = true;
        for (int index = 0; index < count; ++index) {
            winrt::com_ptr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(index, session.put())))
                continue;
            auto control = session.try_as<IAudioSessionControl2>();
            DWORD pid = 0;
            if (!control || control->GetProcessId(&pid) != S_OK || !BelongsToJob(pid, job_))
                continue;
            success = mute(control) && success;
        }
        return success;
    }

    bool mute(const winrt::com_ptr<IAudioSessionControl2>& control) {
        AudioSessionState state{};
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired)
            return true;
        TaskString instance, group;
        auto volume = control.try_as<ISimpleAudioVolume>();
        BOOL wasMuted = FALSE;
        if (!volume || FAILED(volume->GetMute(&wasMuted))
            || FAILED(control->GetSessionInstanceIdentifier(&instance.value))
            || FAILED(control->GetSessionIdentifier(&group.value)))
            return false;

        auto entry = changed_.find(instance.value);
        if (entry != changed_.end())
            return wasMuted || SUCCEEDED(volume->SetMute(TRUE, nullptr));

        // A new stream can inherit the mute from a previous instance of the same audio session.
        if (wasMuted && std::none_of(changed_.begin(), changed_.end(),
            [&](const auto& item) { return item.second.group == group.value; }))
            return true;
        auto saved = changed_.emplace(instance.value, MutedSession{group.value, control, volume}).first;
        if (!wasMuted && FAILED(volume->SetMute(TRUE, nullptr))) {
            changed_.erase(saved);
            return false;
        }
        for (auto old = changed_.begin(); old != changed_.end();) {
            AudioSessionState oldState{};
            if (old != saved && old->second.group == saved->second.group
                && SUCCEEDED(old->second.control->GetState(&oldState)) && oldState == AudioSessionStateExpired) {
                old = changed_.erase(old);
            } else {
                ++old;
            }
        }
        return true;
    }

    HANDLE job_;
    winrt::com_ptr<IMMDeviceEnumerator> devices_;
    std::unordered_map<std::wstring, MutedSession> changed_;
};

} // namespace

AppAudio::AppAudio(HANDLE job, bool muted) : muted_(muted),
    worker_([this, job](std::stop_token stop) {
        std::unique_ptr<AudioSessions> sessions;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) {
            failed_.store(true);
            return;
        }
        while (!stop.stop_requested()) {
            try {
                if (muted_.load()) {
                    if (!sessions)
                        sessions = std::make_unique<AudioSessions>(job);
                    failed_.store(!sessions->apply());
                } else {
                    failed_.store(sessions && !sessions->restore());
                }
            } catch (...) {
                failed_.store(true);
            }
            std::this_thread::sleep_for(200ms);
        }
        sessions.reset();
        CoUninitialize();
    }) {}

void AppAudio::setMuted(bool muted) {
    muted_.store(muted);
}
