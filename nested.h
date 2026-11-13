#pragma once

#include "profile.h"
#include <functional>

constexpr int panelWidth = 624;
constexpr int panelHeight = 352;
constexpr int statusLength = 384;

struct PanelFrame {
    volatile LONG sequence;
    BOOL visible;
    int width;
    int height;
    std::uint8_t pixels[panelWidth * panelHeight * 4];
};

struct NestedDisplay {
    SessionProfile profile;
    volatile LONG statusSequence;
    wchar_t status[statusLength];
    volatile LONG previewVisible;
    alignas(8) volatile LONG64 captureStartedAt;
    PanelFrame game;
};

struct NestedFrames {
    std::function<bool()> enabled;
    std::function<void(const Frame*, bool hideName)> publish;
    std::function<void(bool)> captureBusy = [](bool) {};
};

enum class WorkflowMode { TestSetup, RestartLoop, FinishLoop };
inline constexpr std::array<std::wstring_view, 3> workerModes{L"check", L"run", L"finish"};

bool BlizzardRunning();
std::wstring CreateDesktopName();

class NestedSession {
public:
    NestedSession(const Profile& profile, WorkflowMode mode);
    ~NestedSession();
    NestedSession(const NestedSession&) = delete;
    NestedSession& operator=(const NestedSession&) = delete;
    void stop();
    void setMuted(bool muted);
    void setPreviewVisible(bool visible);
    bool audioFailed() const;
    bool ended() const;
    bool captureTimedOut() const;
    DWORD exitCode() const;
    std::wstring status() const;
    bool readPanel(Frame& frame, LONG& lastSequence) const;
private:
    class State;
    std::unique_ptr<State> state;
};

int RunNestedWorker(const std::wstring& key, WorkflowMode mode);
