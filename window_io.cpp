#include "window_io.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

namespace capture = winrt::Windows::Graphics::Capture;
namespace dx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Graphics::SizeInt32;
using Clock = std::chrono::steady_clock;

namespace {

struct WindowQuery {
    App app;
    HWND result = nullptr;
};

BOOL CALLBACK FindWindowForApp(HWND window, LPARAM argument) {
    auto& query = *reinterpret_cast<WindowQuery*>(argument);
    if (!IsWindowVisible(window))
        return TRUE;

    wchar_t title[256]{}, className[128]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (query.app == App::Game) {
        if (wcscmp(title, L"World of Warcraft") || wcscmp(className, L"waApplication Window"))
            return TRUE;
    } else if (wcscmp(title, L"Battle.net") || wcscmp(className, L"Chrome_WidgetWin_0")) {
        return TRUE;
    }

    try {
        auto path = Process(window).path();
        std::transform(path.begin(), path.end(), path.begin(), towlower);
        if (query.app == App::Game
            ? !path.ends_with(L"\\wow.exe") || path.find(L"\\_forever_\\") == std::wstring::npos
            : !path.ends_with(L"\\battle.net.exe")) {
            return TRUE;
        }
    } catch (const UserError&) {
        return TRUE;
    }
    query.result = window;
    return FALSE;
}

void Post(HWND target, UINT message, WPARAM value, LPARAM data) {
    if (!PostMessageW(target, message, value, data))
        throw UserError{L"Windows could not send background input to the target window."};
}

std::pair<HWND, LPARAM> MouseTarget(HWND root, POINT point, bool useChild) {
    RECT bounds{};
    if (!GetClientRect(root, &bounds) || !PtInRect(&bounds, point))
        throw UserError{L"The click is outside the target window."};
    HWND target = root;
    if (useChild) {
        while (HWND child = ChildWindowFromPointEx(target, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT)) {
            if (child == target)
                break;
            MapWindowPoints(target, child, &point, 1);
            target = child;
        }
    }
    return {target, MAKELPARAM(point.x, point.y)};
}

RECT ClientOnScreen(HWND window) {
    RECT result{};
    POINT origin{};
    if (!GetClientRect(window, &result) || !ClientToScreen(window, &origin))
        throw UserError{L"Could not read the window's client area."};
    OffsetRect(&result, origin.x, origin.y);
    return result;
}

bool SameSize(const RECT& bounds, const SizeInt32& size) {
    return bounds.right - bounds.left == size.Width && bounds.bottom - bounds.top == size.Height;
}

void OpenInCurrentDesktop(const std::filesystem::path& path, const std::wstring& arguments, HANDLE job) {
    if (!std::filesystem::is_regular_file(path))
        throw UserError{L"Could not find the selected launcher. Check the installation in Setup."};
    std::wstring command = L"\"" + path.wstring() + L"\"";
    if (!arguments.empty())
        command += L" " + arguments;
    wchar_t desktop[256]{};
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, nullptr))
        throw UserError{L"Could not identify the Windows desktop for the launcher."};
    std::wstring destination = L"WinSta0\\" + std::wstring(desktop);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.lpDesktop = destination.data();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        path.parent_path().c_str(), &startup, &process)) {
        throw UserError{L"Could not open " + path.filename().wstring()};
    }
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == DWORD(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw UserError{L"Could not start the launcher on the hidden desktop. Windows error " + std::to_wstring(error) + L"."};
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

RECT ClientCrop(HWND window, SizeInt32 size) {
    const RECT client = ClientOnScreen(window);
    if (SameSize(client, size))
        return {0, 0, size.Width, size.Height};

    RECT captureBounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &captureBounds, sizeof captureBounds))
        || !SameSize(captureBounds, size)) {
        if (!GetWindowRect(window, &captureBounds) || !SameSize(captureBounds, size))
            throw UserError{L"Could not match the captured image to the window bounds."};
    }
    RECT crop = client;
    OffsetRect(&crop, -captureBounds.left, -captureBounds.top);
    if (crop.left < 0 || crop.top < 0 || crop.right > size.Width || crop.bottom > size.Height)
        throw UserError{L"Could not capture the entire window."};
    return crop;
}

} // namespace

HWND FindApp(App app) {
    WindowQuery query{app};
    EnumWindows(FindWindowForApp, reinterpret_cast<LPARAM>(&query));
    return query.result;
}

void OpenLauncher(HANDLE job, const std::wstring& path) {
    OpenInCurrentDesktop(path, L"", job);
}

void OpenWoWForever(HANDLE job, const std::wstring& path) {
    OpenInCurrentDesktop(path, LR"(--productcode=wowf --exec="launch WoWF")", job);
}

Process::Process(HWND window, DWORD extraAccess) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid)
        handle_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | extraAccess, FALSE, pid);
    if (!handle_)
        throw UserError{L"Could not check whether the target app closed."};
}

Process::~Process() {
    CloseHandle(handle_);
}

std::wstring Process::path() const {
    std::wstring path(1024, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(handle_, 0, path.data(), &length))
        throw UserError{L"Could not read the target app's file path."};
    path.resize(length);
    return path;
}

void CheckStop(std::stop_token stop) {
    if (stop.stop_requested())
        throw Cancelled{};
}

void Pause(std::stop_token stop, std::chrono::milliseconds delay) {
    const auto deadline = Clock::now() + delay;
    while (Clock::now() < deadline) {
        CheckStop(stop);
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + 50ms));
    }
    CheckStop(stop);
}

void RequireBackground(HWND window) {
    if (!IsWindow(window))
        throw UserError{L"The target window closed."};
    if (GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == window || GetForegroundWindow() == window)
        throw UserError{L"The target window took focus. Automation stopped."};
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (!IsWindowVisible(window) || IsIconic(window) || cloaked)
        throw UserError{L"The target window is hidden or minimized."};
}

void RequestWindowClose(HWND window) {
    RequireBackground(window);
    Post(window, WM_CLOSE, 0, 0);
}

void MovePointerInWindow(HWND window, POINT point, bool useChild) {
    RequireBackground(window);
    const auto [target, coordinates] = MouseTarget(window, point, useChild);
    Post(target, WM_MOUSEMOVE, 0, coordinates);
}

void ClickInWindow(HWND window, POINT point, bool useChild, std::stop_token stop) {
    MovePointerInWindow(window, point, useChild);
    Pause(stop, 180ms);
    RequireBackground(window);
    const auto [target, coordinates] = MouseTarget(window, point, useChild);
    Post(target, WM_LBUTTONDOWN, MK_LBUTTON, coordinates);
    try {
        Pause(stop, 70ms);
    } catch (const Cancelled&) {
        PostMessageW(target, WM_LBUTTONUP, 0, coordinates);
        throw;
    }
    Post(target, WM_LBUTTONUP, 0, coordinates);
}

void TypeInWindow(HWND window, const std::wstring& text, std::stop_token stop) {
    for (const wchar_t ch : text) {
        RequireBackground(window);
        Post(window, WM_CHAR, ch, 1);
        Pause(stop, 65ms);
    }
}

void PressInWindow(HWND window, UINT key, std::stop_token stop) {
    RequireBackground(window);
    const LPARAM state = 1 | (static_cast<LPARAM>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC)) << 16);
    Post(window, WM_KEYDOWN, key, state);
    try {
        Pause(stop, 65ms);
    } catch (const Cancelled&) {
        PostMessageW(window, WM_KEYUP, key, state | 0xC0000000);
        throw;
    }
    Post(window, WM_KEYUP, key, state | 0xC0000000);
}

std::wstring SaveDiagnosticFrame(const Frame& frame, const wchar_t* fileName) {
    wchar_t* raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw UserError{L"Windows could not find your local settings folder."};
    const std::filesystem::path folder = std::filesystem::path(raw) / L"WoW Nested";
    CoTaskMemFree(raw);
    std::filesystem::create_directories(folder);
    const auto path = (folder / fileName).wstring();

    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> image;
    winrt::check_hresult(encoder->CreateNewFrame(image.put(), nullptr));
    winrt::check_hresult(image->Initialize(nullptr));
    winrt::check_hresult(image->SetSize(frame.width, frame.height));
    // Captured pixels carry no meaningful alpha, so the image is stored as 24-bit color.
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(frame.width) * frame.height * 3);
    for (std::size_t i = 0, n = static_cast<std::size_t>(frame.width) * frame.height; i < n; ++i)
        std::memcpy(rgb.data() + i * 3, frame.pixels.data() + i * 4, 3);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    winrt::check_hresult(image->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR)
        throw UserError{L"Could not save the WoW screen image."};
    winrt::check_hresult(image->WritePixels(frame.height, frame.width * 3, static_cast<UINT>(rgb.size()), rgb.data()));
    winrt::check_hresult(image->Commit());
    winrt::check_hresult(encoder->Commit());
    return path;
}

class WindowCapture::State {
public:
    explicit State(HWND target) : window(target) {
        RequireBackground(window);
        if (!capture::GraphicsCaptureSession::IsSupported())
            throw UserError{L"Windows does not support window capture on this device."};

        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(),
            nullptr, context.put()));

        const auto dxgiDevice = device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        graphicsDevice = inspectable.as<dx::Direct3D11::IDirect3DDevice>();

        const auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(window,
            winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        size = item.Size();
        pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(graphicsDevice,
            dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        session.StartCapture();
    }

    ~State() {
        if (session) {
            try { session.Close(); } catch (...) {}
        }
        if (pool) {
            try { pool.Close(); } catch (...) {}
        }
    }

    const Frame* latest() {
        RequireBackground(window);
        auto next = pool.TryGetNextFrame();
        if (!next)
            return frame.pixels.empty() ? nullptr : &frame;
        if (auto newer = pool.TryGetNextFrame())
            next = std::move(newer);
        const auto content = next.ContentSize();
        if (content.Width != size.Width || content.Height != size.Height) {
            next.Close();
            size = content;
            pool.Recreate(graphicsDevice, dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            frame = {};
            return nullptr;
        }

        const auto access = next.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        const RECT crop = ClientCrop(window, size);
        const int width = crop.right - crop.left;
        const int height = crop.bottom - crop.top;
        if (!staging || frame.width != width || frame.height != height) {
            D3D11_TEXTURE2D_DESC descriptor{};
            texture->GetDesc(&descriptor);
            descriptor.Width = width;
            descriptor.Height = height;
            descriptor.MipLevels = descriptor.ArraySize = 1;
            descriptor.Usage = D3D11_USAGE_STAGING;
            descriptor.BindFlags = descriptor.MiscFlags = 0;
            descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging = nullptr;
            winrt::check_hresult(device->CreateTexture2D(&descriptor, nullptr, staging.put()));
            frame = {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        }

        const D3D11_BOX area{static_cast<UINT>(crop.left), static_cast<UINT>(crop.top), 0,
                             static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &area);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        for (int row = 0; row < height; ++row)
            std::memcpy(frame.pixels.data() + row * stride, source + row * mapped.RowPitch, stride);
        context->Unmap(staging.get(), 0);
        return &frame;
    }

private:
    HWND window;
    Frame frame;
    SizeInt32 size{};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    dx::Direct3D11::IDirect3DDevice graphicsDevice{nullptr};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
};

WindowCapture::WindowCapture(HWND window) : state_(std::make_unique<State>(window)) {}
WindowCapture::~WindowCapture() = default;
const Frame* WindowCapture::Latest() { return state_->latest(); }

HiddenCapture::HiddenCapture(HWND window) : window_(window), dc_(CreateCompatibleDC(nullptr)) {
    if (!dc_)
        throw UserError{L"Could not open the hidden desktop's capture surface."};
}

HiddenCapture::~HiddenCapture() {
    if (bitmap_) {
        SelectObject(dc_, original_);
        DeleteObject(bitmap_);
    }
    DeleteDC(dc_);
}

const Frame* HiddenCapture::Latest() {
    RECT bounds{};
    if (!GetClientRect(window_, &bounds) || bounds.right <= 0 || bounds.bottom <= 0)
        return nullptr;
    const int width = bounds.right, height = bounds.bottom;
    if (width > 4096 || height > 2160)
        throw UserError{L"The window exceeds the supported capture size."};
    if (!bitmap_ || frame_.width != width || frame_.height != height) {
        if (bitmap_) {
            SelectObject(dc_, original_);
            DeleteObject(bitmap_);
        }
        BITMAPINFO format{};
        format.bmiHeader.biSize = sizeof format.bmiHeader;
        format.bmiHeader.biWidth = width;
        format.bmiHeader.biHeight = -height;
        format.bmiHeader.biPlanes = 1;
        format.bmiHeader.biBitCount = 32;
        bitmap_ = CreateDIBSection(dc_, &format, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        if (!bitmap_)
            throw UserError{L"Could not allocate memory for the window capture."};
        original_ = SelectObject(dc_, bitmap_);
        frame_ = {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    }
    constexpr UINT renderFullContent = 2;
    if (!PrintWindow(window_, dc_, PW_CLIENTONLY | renderFullContent))
        return nullptr;
    GdiFlush();
    std::memcpy(frame_.pixels.data(), pixels_, frame_.pixels.size());
    return &frame_;
}

ImageTemplate::ImageTemplate(int resourceId, bool masked) {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource)
        throw UserError{L"A built-in reference image is missing."};
    const auto blob = LoadResource(nullptr, resource);
    if (!blob)
        throw UserError{L"Could not open a built-in reference image."};

    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(blob)),
        SizeofResource(nullptr, resource)));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr,
        WICDecodeMetadataCacheOnLoad, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> image;
    winrt::check_hresult(decoder->GetFrame(0, image.put()));
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(image.get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT width = 0, height = 0;
    winrt::check_hresult(converter->GetSize(&width, &height));
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    std::vector<std::uint8_t> bytes(width * height * 4);
    winrt::check_hresult(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes.size()), bytes.data()));

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const auto* px = bytes.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
            if (masked && px[0] == 255 && px[1] == 0 && px[2] == 255)
                continue;
            samples_.push_back({x, y, px[0], px[1], px[2]});
        }
    }
    if (samples_.empty())
        throw UserError{L"The reference image has no pixels to match."};

    // Area-average the image to three quarters of its size for 1920 × 1080 frames. A reference pixel at
    // x lands at 0.75 x, so each of the four alignments per axis gets its own reduced copy. A reduced
    // pixel is used only when most of the source area it covers is unmasked.
    for (int phaseY = 0; phaseY < 4; ++phaseY) {
        for (int phaseX = 0; phaseX < 4; ++phaseX) {
            auto& variant = compact_[phaseY * 4 + phaseX];
            variant.width = (3 * (width_ + phaseX) + 3) / 4;
            variant.height = (3 * (height_ + phaseY) + 3) / 4;
            for (int cy = 0; cy < variant.height; ++cy) {
                for (int cx = 0; cx < variant.width; ++cx) {
                    const double left = cx * 4.0 / 3 - phaseX, right = (cx + 1) * 4.0 / 3 - phaseX;
                    const double top = cy * 4.0 / 3 - phaseY, bottom = (cy + 1) * 4.0 / 3 - phaseY;
                    double sum[3]{}, weight = 0;
                    const int lastY = std::min<int>(height_, int(std::ceil(bottom)));
                    const int lastX = std::min<int>(width_, int(std::ceil(right)));
                    for (int sy = std::max(0, int(std::floor(top))); sy < lastY; ++sy) {
                        for (int sx = std::max(0, int(std::floor(left))); sx < lastX; ++sx) {
                            const auto* px = bytes.data() + (static_cast<std::size_t>(sy) * width_ + sx) * 4;
                            if (masked && px[0] == 255 && px[1] == 0 && px[2] == 255)
                                continue;
                            const double w = (std::min<double>(sx + 1, right) - std::max<double>(sx, left))
                                           * (std::min<double>(sy + 1, bottom) - std::max<double>(sy, top));
                            for (int c = 0; c < 3; ++c)
                                sum[c] += w * px[c];
                            weight += w;
                        }
                    }
                    if (weight < 16.0 / 9 * 0.75)
                        continue;
                    variant.samples.push_back({cx, cy, static_cast<std::uint8_t>(sum[0] / weight + 0.5),
                        static_cast<std::uint8_t>(sum[1] / weight + 0.5), static_cast<std::uint8_t>(sum[2] / weight + 0.5)});
                }
            }
            if (variant.samples.empty())
                throw UserError{L"The reference image has no pixels to match."};
        }
    }

    // Check widely spaced pixels first so mismatches stop early.
    std::vector<Sample> ordered;
    ordered.reserve(samples_.size());
    constexpr std::size_t stride = 47;
    for (std::size_t start = 0; start < stride; ++start)
        for (std::size_t i = start; i < samples_.size(); i += stride)
            ordered.push_back(samples_[i]);
    samples_ = std::move(ordered);
}

bool ImageTemplate::Matches(const Frame& frame, RECT region, int tolerance) const {
    const int lastX = std::min<int>(region.right, frame.width) - width_;
    const int lastY = std::min<int>(region.bottom, frame.height) - height_;
    for (int y = std::max<LONG>(0, region.top); y <= lastY; ++y) {
        for (int x = std::max<LONG>(0, region.left); x <= lastX; ++x) {
            const auto origin = (static_cast<std::size_t>(y) * frame.width + x) * 4;
            const bool match = std::all_of(samples_.begin(), samples_.end(), [&](const Sample& sample) {
                const auto index = origin + (static_cast<std::size_t>(sample.y) * frame.width + sample.x) * 4;
                const auto* px = frame.pixels.data() + index;
                return std::abs(int(px[0]) - sample.blue) <= tolerance
                    && std::abs(int(px[1]) - sample.green) <= tolerance
                    && std::abs(int(px[2]) - sample.red) <= tolerance;
            });
            if (match)
                return true;
        }
    }
    return false;
}

double ImageTemplate::CompactScore(const Frame& frame, RECT region) const {
    double best = std::numeric_limits<double>::max();
    for (int phase = 0; phase < 16; ++phase) {
        const auto& variant = compact_[phase];
        const int phaseX = phase % 4, phaseY = phase / 4;
        // Reference positions x with x % 4 == phaseX start at 0.75 (x - phaseX) in the compact frame.
        const auto first = [](LONG value, int phaseValue) {
            const LONG aligned = value - phaseValue;
            return std::max<LONG>(0, (aligned + 3) / 4 * 3);
        };
        const int left = first(std::max<LONG>(phaseX, region.left), phaseX);
        const int top = first(std::max<LONG>(phaseY, region.top), phaseY);
        const int lastX = std::min<int>(frame.width - variant.width, (region.right - width_ - phaseX) / 4 * 3);
        const int lastY = std::min<int>(frame.height - variant.height, (region.bottom - height_ - phaseY) / 4 * 3);
        if (region.right - width_ - phaseX < 0 || region.bottom - height_ - phaseY < 0)
            continue;
        const long long limitScale = 3LL * variant.samples.size();
        for (int y = top; y <= lastY; y += 3) {
            for (int x = left; x <= lastX; x += 3) {
                const auto origin = (static_cast<std::size_t>(y) * frame.width + x) * 4;
                const long long limit = best == std::numeric_limits<double>::max()
                    ? std::numeric_limits<long long>::max() : static_cast<long long>(best * limitScale);
                long long total = 0;
                for (const auto& sample : variant.samples) {
                    const auto* px = frame.pixels.data() + origin
                        + (static_cast<std::size_t>(sample.y) * frame.width + sample.x) * 4;
                    total += std::abs(int(px[0]) - sample.blue) + std::abs(int(px[1]) - sample.green)
                           + std::abs(int(px[2]) - sample.red);
                    if (total > limit)
                        break;
                }
                best = std::min(best, double(total) / limitScale);
            }
        }
    }
    return best;
}
