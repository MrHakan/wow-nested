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
    PanelFrame game;
};

struct NestedFrames {
    std::function<void(const Frame*)> publish;
};

bool BlizzardRunning();
std::wstring CreateDesktopName();

class NestedSession {
public:
    NestedSession(const Profile& profile, bool checkOnly);
    ~NestedSession();
    NestedSession(const NestedSession&) = delete;
    NestedSession& operator=(const NestedSession&) = delete;
    void stop();
    void setMuted(bool muted);
    bool audioFailed() const;
    bool ended() const;
    DWORD exitCode() const;
    std::wstring status() const;
    bool readPanel(Frame& frame, LONG& lastSequence) const;
private:
    class State;
    std::unique_ptr<State> state;
};

int RunNestedWorker(const std::wstring& key, bool checkOnly);
