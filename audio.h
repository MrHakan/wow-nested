#pragma once

#include "window_io.h"
#include <atomic>
#include <thread>

class AppAudio {
public:
    AppAudio(HANDLE job, bool muted);
    void setMuted(bool muted);
    bool failed() const { return failed_.load(); }
private:
    std::atomic<bool> muted_;
    std::atomic<bool> failed_{false};
    std::jthread worker_;
};
