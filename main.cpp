#include "nested.h"
#include "setup.h"
#include <shellapi.h>
#include <algorithm>

namespace {

constexpr int testButtonId = 101;
constexpr int startButtonId = 102;
constexpr int stopButtonId = 103;
constexpr int muteButtonId = 104;
constexpr int setupButtonId = 105;
constexpr int modePickerId = 106;
constexpr COLORREF page = RGB(242, 246, 245);
constexpr COLORREF ink = RGB(20, 34, 38);
constexpr COLORREF muted = RGB(78, 102, 104);
constexpr COLORREF accent = RGB(10, 102, 91);
constexpr COLORREF video = RGB(21, 36, 41);

HFONT Font(int pixels, int weight = FW_NORMAL) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Inter");
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT format) {
    const auto previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

class Dashboard {
public:
    Dashboard(HINSTANCE instance, Profile profile) : instance(instance), profile(std::move(profile)) {}

    int run() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof windowClass;
        windowClass.lpfnWndProc = dispatch;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        windowClass.lpszClassName = L"WoWBackgroundDashboard";
        if (!RegisterClassExW(&windowClass))
            return 1;

        window = CreateWindowExW(0, windowClass.lpszClassName, L"WoW Nested", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 920, 565, nullptr, nullptr, instance, this);
        if (!window)
            return 1;
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return 0;
    }

    ~Dashboard() {
        for (HFONT font : {titleFont, bodyFont, labelFont, smallFont})
            DeleteObject(font);
    }

private:
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<Dashboard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Dashboard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            modePicker = CreateWindowExW(0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(modePickerId)), instance, nullptr);
            SendMessageW(modePicker, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), FALSE);
            for (const auto* mode : retryModes)
                SendMessageW(modePicker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode));
            updateModePicker();
            testButton = button(L"Test setup", testButtonId);
            startButton = button(L"Start", startButtonId);
            stopButton = button(L"Stop", stopButtonId);
            muteButton = button(profile.muted ? L"Unmute audio" : L"Mute audio", muteButtonId);
            setupButton = button(L"Settings", setupButtonId);
            EnableWindow(stopButton, FALSE);
            SetTimer(window, 1, 200, nullptr);
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                updatePreview(false);
            else
                layout();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
                updatePreview(false);
            break;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {800, 500};
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_PRINTCLIENT:
            paintContent(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_TIMER:
            poll();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case testButtonId: start(WorkflowMode::TestSetup); return 0;
            case startButtonId:
                start(profile.retryMode == RetryMode::Loop ? WorkflowMode::FinishLoop : WorkflowMode::RestartLoop);
                return 0;
            case modePickerId:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                    selectMode();
                return 0;
            case stopButtonId: stop(); return 0;
            case muteButtonId: toggleAudio(); return 0;
            case setupButtonId: openSetup(); return 0;
            default: break;
            }
            break;
        case WM_CLOSE:
            if (session || recovering) {
                closing = true;
                stop();
            } else {
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        default: break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND button(const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont), FALSE);
        return control;
    }

    void layout() {
        if (!testButton)
            return;
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right;
        const int height = client.bottom;
        const int split = 240;
        previewLauncher = {26, 151, split, height - 146};
        previewGame = {split + 20, 151, width - 26, height - 146};
        statusRect = {26, height - 127, width - 26, height - 73};
        MoveWindow(modePicker, width - 252, 47, 226, 130, TRUE);
        MoveWindow(testButton, 26, height - 57, 116, 36, TRUE);
        MoveWindow(startButton, 152, height - 57, 100, 36, TRUE);
        MoveWindow(stopButton, 262, height - 57, 80, 36, TRUE);
        MoveWindow(muteButton, 352, height - 57, 134, 36, TRUE);
        MoveWindow(setupButton, 496, height - 57, 92, 36, TRUE);
        poll();
        InvalidateRect(window, nullptr, FALSE);
    }

    void poll() {
        if (!session) {
            if (recovering && GetTickCount64() >= retryAt) {
                try {
                    openSession();
                    recovering = false;
                } catch (const UserError& error) {
                    scheduleRecovery(error.message);
                }
            }
            return;
        }
        if (const bool failed = session->audioFailed(); failed != audioFailed) {
            audioFailed = failed;
            InvalidateRect(window, nullptr, FALSE);
        }
        updatePreview(!stopping && !IsIconic(window) && GetForegroundWindow() == window);
        if (previewVisible && session->readPanel(gameFrame, gameSequence))
            InvalidateRect(window, &previewGame, FALSE);
        if (!stopping) {
            const auto message = session->status();
            if (!message.empty() && message != status)
                setStatus(message);
        }
        if (session->ended() || (stopping && GetTickCount64() - stoppingAt >= 2000)) {
            const DWORD code = session->exitCode();
            session.reset();
            gameFrame = {};
            previewVisible = false;
            audioFailed = false;
            InvalidateRect(window, nullptr, FALSE);
            if (stopping)
                setStatus(L"Stopped. The apps on the hidden desktop were closed.");
            else if (code != 0)
                setStatus(L"The hidden desktop worker stopped. Windows error " + std::to_wstring(code) + L".");
            finishSession();
        } else if (!stopping && session->captureTimedOut()) {
            session.reset();
            gameFrame = {};
            previewVisible = false;
            audioFailed = false;
            if (activeMode == WorkflowMode::TestSetup) {
                setStatus(L"WoW screen capture did not respond for 35 seconds.");
                finishSession();
            } else {
                scheduleRecovery(L"WoW screen capture did not respond for 35 seconds.");
            }
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    void paint() {
        PAINTSTRUCT state{};
        HDC dc = BeginPaint(window, &state);
        paintContent(dc);
        EndPaint(window, &state);
    }

    void paintContent(HDC dc) {
        RECT client{};
        GetClientRect(window, &client);
        Fill(dc, client, page);
        Fill(dc, {0, 0, client.right, 5}, accent);
        Text(dc, L"WoW Nested", {26, 22, client.right - 26, 62}, titleFont, ink, DT_SINGLELINE);
        Text(dc, CharacterSummary(profile),
             {27, 71, client.right - 270, 99}, bodyFont, muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"Retry mode", {client.right - 252, 22, client.right - 26, 44},
             labelFont, ink, DT_SINGLELINE);
        Text(dc, L"Session", {previewLauncher.left, 119, previewLauncher.right, 144},
             labelFont, ink, DT_SINGLELINE);
        Text(dc, L"WoW: Forever", {previewGame.left, 119, previewGame.right, 144},
             labelFont, ink, DT_SINGLELINE);
        Fill(dc, previewLauncher, RGB(228, 237, 234));
        RECT launcherText = previewLauncher;
        InflateRect(&launcherText, -14, -15);
        Text(dc, recovering ? L"Restarting" : session ? L"Running" : L"Ready to start",
             launcherText, labelFont, accent, DT_SINGLELINE);
        launcherText.top += 30;
        Text(dc, L"Account details are hidden.",
             launcherText, bodyFont, muted, DT_WORDBREAK);
        paintPreview(dc, previewGame, gameFrame, !session ? L"WoW is closed"
            : previewVisible ? L"Waiting for the character screen" : L"Preview paused");
        Fill(dc, statusRect, RGB(221, 237, 232));
        RECT detail = statusRect;
        InflateRect(&detail, -12, -8);
        Text(dc, status, detail, bodyFont, ink, DT_WORDBREAK);
        const auto footer = audioFailed ? L"Audio control unavailable"
            : std::wstring(rulesets[int(profile.ruleset)]);
        Text(dc, footer, {601, client.bottom - 57, client.right - 26, client.bottom - 21},
             smallFont, muted, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    void paintPreview(HDC dc, RECT rect, const Frame& frame, const wchar_t* empty) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0)
            return;
        Fill(dc, rect, video);
        if (frame.pixels.empty()) {
            Text(dc, empty, rect, smallFont, RGB(187, 208, 208), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const double scale = std::min(double(width) / frame.width, double(height) / frame.height);
        const int targetWidth = static_cast<int>(frame.width * scale);
        const int targetHeight = static_cast<int>(frame.height * scale);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof bitmap.bmiHeader;
        bitmap.bmiHeader.biWidth = frame.width;
        bitmap.bmiHeader.biHeight = -frame.height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, rect.left + (width - targetWidth) / 2, rect.top + (height - targetHeight) / 2,
            targetWidth, targetHeight, 0, 0, frame.width, frame.height, frame.pixels.data(), &bitmap,
            DIB_RGB_COLORS, SRCCOPY);
    }

    void setStatus(const std::wstring& message) {
        status = message;
        InvalidateRect(window, &statusRect, FALSE);
    }

    void updatePreview(bool visible) {
        if (!session || previewVisible == visible)
            return;
        if (visible) {
            Frame previous;
            session->readPanel(previous, gameSequence);
        } else {
            gameFrame = {};
        }
        previewVisible = visible;
        session->setPreviewVisible(visible);
        InvalidateRect(window, &previewGame, FALSE);
    }

    void updateModePicker() {
        SendMessageW(modePicker, CB_SETCURSEL, static_cast<WPARAM>(profile.retryMode), 0);
    }

    void selectMode() {
        const auto selected = SendMessageW(modePicker, CB_GETCURSEL, 0, 0);
        if (session || recovering || selected < 0 || static_cast<std::size_t>(selected) >= retryModes.size())
            return;
        const auto previous = profile.retryMode;
        profile.retryMode = static_cast<RetryMode>(selected);
        try {
            SaveProfile(profile);
            setStatus(L"Retry mode saved.");
        } catch (const UserError& error) {
            profile.retryMode = previous;
            updateModePicker();
            setStatus(error.message);
        }
    }

    void start(WorkflowMode mode) {
        if (session || recovering)
            return;
        activeMode = mode;
        stopping = false;
        try {
            openSession();
        } catch (const UserError& error) {
            setStatus(error.message);
            return;
        }
        EnableWindow(testButton, FALSE);
        EnableWindow(startButton, FALSE);
        EnableWindow(modePicker, FALSE);
        EnableWindow(stopButton, TRUE);
        EnableWindow(setupButton, FALSE);
    }

    void openSession() {
        session = std::make_unique<NestedSession>(profile, activeMode);
        gameFrame = {};
        gameSequence = 0;
        updatePreview(!IsIconic(window) && GetForegroundWindow() == window);
        setStatus(L"Opening the hidden desktop.");
        InvalidateRect(window, nullptr, FALSE);
    }

    void scheduleRecovery(const std::wstring& reason) {
        recovering = true;
        retryAt = GetTickCount64() + 10000;
        setStatus(reason + L" Restarting the session in 10 seconds.");
    }

    void finishSession() {
        EnableWindow(testButton, TRUE);
        EnableWindow(startButton, TRUE);
        EnableWindow(modePicker, TRUE);
        EnableWindow(stopButton, FALSE);
        EnableWindow(setupButton, TRUE);
        if (closing)
            DestroyWindow(window);
    }

    void stop() {
        if (recovering) {
            recovering = false;
            setStatus(L"Stopped.");
            InvalidateRect(window, nullptr, FALSE);
            finishSession();
            return;
        }
        if (session && !stopping) {
            stopping = true;
            stoppingAt = GetTickCount64();
            updatePreview(false);
            session->stop();
            setStatus(L"Stopping.");
            EnableWindow(stopButton, FALSE);
        }
    }

    void toggleAudio() {
        profile.muted = !profile.muted;
        SetWindowTextW(muteButton, profile.muted ? L"Unmute audio" : L"Mute audio");
        if (session)
            session->setMuted(profile.muted);
        try {
            SaveProfile(profile);
        } catch (const UserError& error) {
            setStatus(error.message);
        }
    }

    void openSetup() {
        if (!session && !recovering && ShowSetup(instance, window, profile)) {
            updateModePicker();
            setStatus(L"Settings saved.");
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    HINSTANCE instance;
    HWND window = nullptr, testButton = nullptr, startButton = nullptr, modePicker = nullptr, stopButton = nullptr;
    HWND muteButton = nullptr, setupButton = nullptr;
    HFONT titleFont = Font(34, FW_BOLD), bodyFont = Font(17), labelFont = Font(16, FW_SEMIBOLD),
          smallFont = Font(15);
    RECT previewLauncher{}, previewGame{}, statusRect{};
    Frame gameFrame;
    LONG gameSequence = 0;
    std::wstring status = L"Close any open WoW and Battle.net windows before starting.";
    std::unique_ptr<NestedSession> session;
    ULONGLONG stoppingAt = 0;
    ULONGLONG retryAt = 0;
    WorkflowMode activeMode = WorkflowMode::TestSetup;
    bool stopping = false;
    bool recovering = false;
    bool closing = false;
    bool audioFailed = false;
    bool previewVisible = false;
    Profile profile;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args && count == 4 && std::wstring_view(args[1]) == L"--nested-worker") {
        const std::wstring name(args[2]);
        const auto mode = std::find(workerModes.begin(), workerModes.end(), std::wstring_view(args[3]));
        LocalFree(args);
        if (mode == workerModes.end())
            return 2;
        return RunNestedWorker(name, static_cast<WorkflowMode>(mode - workerModes.begin()));
    }
    if (args)
        LocalFree(args);
    try {
        Profile profile;
        if (!LoadProfile(profile)) {
            FindInstallations(profile);
            if (!ShowSetup(instance, nullptr, profile))
                return 0;
        }
        return Dashboard(instance, std::move(profile)).run();
    } catch (const UserError& error) {
        MessageBoxW(nullptr, error.message.c_str(), L"WoW Nested", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"WoW Nested could not start.", L"WoW Nested", MB_OK | MB_ICONERROR);
    }
    return 1;
}
