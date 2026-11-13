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
            testButton = button(L"Test setup", testButtonId);
            startButton = button(L"Start loop", startButtonId);
            stopButton = button(L"Stop", stopButtonId);
            muteButton = button(profile.muted ? L"Unmute audio" : L"Mute audio", muteButtonId);
            setupButton = button(L"Setup", setupButtonId);
            EnableWindow(stopButton, FALSE);
            SetTimer(window, 1, 200, nullptr);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
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
            case testButtonId: start(true); return 0;
            case startButtonId: start(false); return 0;
            case stopButtonId: stop(); return 0;
            case muteButtonId: toggleAudio(); return 0;
            case setupButtonId: openSetup(); return 0;
            default: break;
            }
            break;
        case WM_CLOSE:
            if (session) {
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
        MoveWindow(testButton, 26, height - 57, 116, 36, TRUE);
        MoveWindow(startButton, 152, height - 57, 116, 36, TRUE);
        MoveWindow(stopButton, 278, height - 57, 80, 36, TRUE);
        MoveWindow(muteButton, 368, height - 57, 134, 36, TRUE);
        MoveWindow(setupButton, 512, height - 57, 76, 36, TRUE);
        poll();
        InvalidateRect(window, nullptr, FALSE);
    }

    void poll() {
        if (!session)
            return;
        if (const bool failed = session->audioFailed(); failed != audioFailed) {
            audioFailed = failed;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (session->readPanel(gameFrame, gameSequence))
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
            audioFailed = false;
            InvalidateRect(window, nullptr, FALSE);
            if (stopping)
                setStatus(L"Stopped. The apps on the hidden desktop were closed.");
            else if (code != 0)
                setStatus(L"The hidden desktop worker stopped. Windows error " + std::to_wstring(code) + L".");
            EnableWindow(testButton, TRUE);
            EnableWindow(startButton, TRUE);
            EnableWindow(stopButton, FALSE);
            EnableWindow(setupButton, TRUE);
            if (closing)
                DestroyWindow(window);
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
             {27, 71, client.right - 26, 99}, bodyFont, muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"Session", {previewLauncher.left, 119, previewLauncher.right, 144},
             labelFont, ink, DT_SINGLELINE);
        Text(dc, L"WoW: Forever", {previewGame.left, 119, previewGame.right, 144},
             labelFont, ink, DT_SINGLELINE);
        Fill(dc, previewLauncher, RGB(228, 237, 234));
        RECT launcherText = previewLauncher;
        InflateRect(&launcherText, -14, -15);
        Text(dc, session ? L"Running" : L"Ready to start",
             launcherText, labelFont, accent, DT_SINGLELINE);
        launcherText.top += 30;
        Text(dc, L"Account details are hidden.",
             launcherText, bodyFont, muted, DT_WORDBREAK);
        paintPreview(dc, previewGame, gameFrame, session ? L"Waiting for the character screen" : L"WoW is closed");
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

    void start(bool test) {
        if (session)
            return;
        try {
            session = std::make_unique<NestedSession>(profile, test);
        } catch (const UserError& error) {
            setStatus(error.message);
            return;
        }
        stopping = false;
        gameFrame = {};
        gameSequence = 0;
        EnableWindow(testButton, FALSE);
        EnableWindow(startButton, FALSE);
        EnableWindow(stopButton, TRUE);
        EnableWindow(setupButton, FALSE);
        setStatus(L"Opening the hidden desktop.");
        InvalidateRect(window, nullptr, FALSE);
    }

    void stop() {
        if (session && !stopping) {
            stopping = true;
            stoppingAt = GetTickCount64();
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
        if (!session && ShowSetup(instance, window, profile)) {
            setStatus(L"Setup saved.");
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    HINSTANCE instance;
    HWND window = nullptr, testButton = nullptr, startButton = nullptr, stopButton = nullptr;
    HWND muteButton = nullptr, setupButton = nullptr;
    HFONT titleFont = Font(34, FW_BOLD), bodyFont = Font(17), labelFont = Font(16, FW_SEMIBOLD),
          smallFont = Font(15);
    RECT previewLauncher{}, previewGame{}, statusRect{};
    Frame gameFrame;
    LONG gameSequence = 0;
    std::wstring status = L"Close any open WoW and Battle.net windows before starting.";
    std::unique_ptr<NestedSession> session;
    ULONGLONG stoppingAt = 0;
    bool stopping = false;
    bool closing = false;
    bool audioFailed = false;
    Profile profile;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args && count == 4 && std::wstring_view(args[1]) == L"--nested-worker") {
        const std::wstring name(args[2]);
        const bool checkOnly = std::wstring_view(args[3]) == L"check";
        LocalFree(args);
        return RunNestedWorker(name, checkOnly);
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
