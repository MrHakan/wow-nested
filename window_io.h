#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

using namespace std::chrono_literals;

struct UserError {
    std::wstring message;
};

struct Cancelled {};

enum class App { Launcher, Game };

HWND FindApp(App app);
void OpenLauncher(HANDLE job, const std::wstring& path);
void OpenWoWForever(HANDLE job, const std::wstring& path);
bool ProcessRunning(HANDLE process);
void WaitForExit(HANDLE process, std::chrono::milliseconds timeout, std::stop_token stop,
                 const std::wstring& error);
void CheckStop(std::stop_token stop);
void Pause(std::stop_token stop, std::chrono::milliseconds delay);
void RequireBackground(HWND window);
void RequestWindowClose(HWND window);
void MovePointerInWindow(HWND window, POINT point, bool useChild);
void ClickInWindow(HWND window, POINT point, bool useChild, std::stop_token stop);
void TypeInWindow(HWND window, const std::wstring& text, std::stop_token stop);
void PressInWindow(HWND window, UINT key, std::stop_token stop);

class Process {
public:
    explicit Process(HWND window, DWORD extraAccess = 0);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    HANDLE get() const { return handle_; }
    std::wstring path() const;
private:
    HANDLE handle_ = nullptr;
};

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

class WindowCapture {
public:
    explicit WindowCapture(HWND window);
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;
    const Frame* Latest();
private:
    class State;
    std::unique_ptr<State> state_;
};

class HiddenCapture {
public:
    explicit HiddenCapture(HWND window);
    ~HiddenCapture();
    HiddenCapture(const HiddenCapture&) = delete;
    HiddenCapture& operator=(const HiddenCapture&) = delete;
    const Frame* Latest();
private:
    HWND window_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    void* pixels_ = nullptr;
    Frame frame_;
};

class ImageTemplate {
public:
    explicit ImageTemplate(int resourceId, bool masked = false);
    bool Matches(const Frame& frame, RECT region, int tolerance) const;
private:
    struct Sample {
        int x;
        int y;
        std::uint8_t blue;
        std::uint8_t green;
        std::uint8_t red;
    };
    int width_ = 0;
    int height_ = 0;
    std::vector<Sample> samples_;
};
