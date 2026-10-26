#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "capture_options.hpp"
#include "sender.hpp"
#include "resolution.hpp"
#include "cursor.hpp"
#include <mutex>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

#ifndef FRAME_SENDER_VERSION
#define FRAME_SENDER_VERSION "development"
#endif

namespace {
constexpr uint16_t frame_stride = 1352;
constexpr size_t frame_header_size = 48;

class HrError : public std::runtime_error {
  public:
    HRESULT value;
    HrError(HRESULT hr, const std::string& action) : std::runtime_error(message(hr, action)), value(hr) {}

  private:
    static std::string message(HRESULT hr, const std::string& action) {
        std::ostringstream out;
        out << action << " failed (0x" << std::hex << std::uppercase << uint32_t(hr) << ')';
        return out.str();
    }
};

void checked(HRESULT hr, const char* action) {
    if (FAILED(hr))
        throw HrError(hr, action);
}

void put16(uint8_t* p, uint16_t value) {
    p[0] = uint8_t(value >> 8);
    p[1] = uint8_t(value);
}

void put32(uint8_t* p, uint32_t value) {
    put16(p, uint16_t(value >> 16));
    put16(p + 2, uint16_t(value));
}

void put64(uint8_t* p, uint64_t value) {
    put32(p, uint32_t(value >> 32));
    put32(p + 4, uint32_t(value));
}

uint16_t get16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}

uint32_t get32(const uint8_t* p) {
    return uint32_t(get16(p)) << 16 | get16(p + 2);
}

uint64_t get64(const uint8_t* p) {
    return uint64_t(get32(p)) << 32 | get32(p + 4);
}

int64_t qpc_to_ns(int64_t ticks) {
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0)
            throw std::runtime_error("QueryPerformanceFrequency failed");
        return value.QuadPart;
    }();
    const int64_t seconds = ticks / frequency;
    const int64_t remainder = ticks % frequency;
    return seconds * 1'000'000'000ll + remainder * 1'000'000'000ll / frequency;
}

int64_t now_ns() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return qpc_to_ns(value.QuadPart);
}

uint64_t random_session() {
    uint64_t value = 0;
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Windows random generator failed");
    } while (!value);
    return value;
}

std::string utf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
        return {};
    std::string result(size_t(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    result.resize(size_t(count - 1));
    return result;
}

using frame_sender::Options;

unsigned integer(const std::string& text, const char* name, unsigned maximum) {
    size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &used);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    }
    if (used != text.size() || value > maximum)
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    return unsigned(value);
}

double real(const std::string& text, const char* name, double maximum) {
    size_t used = 0;
    double value = 0;
    try {
        value = std::stod(text, &used);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    }
    if (used != text.size() || !std::isfinite(value) || value < 0 || value > maximum)
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    return value;
}

void usage() {
    std::cout << "FrameSender " FRAME_SENDER_VERSION
                 " - low-latency, center-crop-only Windows frame sender\n\n"
              << "frame_sender --host RECEIVER_IPV4 [options]\n\n"
              << "  --sync-resolution       follow Receiver's capture size (default)\n"
              << "  --no-sync-resolution    use a fixed crop; supports older Receivers\n"
              << "  --size PIXELS           manual square crop (default 320)\n"
              << "  --width PIXELS          manual crop width, 1..1024\n"
              << "  --height PIXELS         manual crop height, 1..1024\n"
              << "  --output INDEX          desktop output from --list-outputs\n"
              << "  --capture METHOD        dxgi (default), wgc, or gdi\n"
              << "  --list-capture-methods  describe capture methods and exit\n"
              << "  --fps FPS               send cap; 0 = new frames (GDI polls at 60)\n"
              << "  --port PORT             Receiver frame port (default 5000)\n"
              << "  --send-buffer-mib MIB   UDP queue, 1..16 (default 2)\n"
              << "  --list-outputs          list displays and exit\n"
              << "  --self-test             verify wire encoding and exit\n"
              << "  --version               show the software version\n"
              << "  --help                  show this text\n\n";
    for (const auto& info : frame_sender::capture_methods)
        std::cout << "  " << info.name << ": " << info.description << '\n';
}

Options parse_options(int argc, char** argv) {
    Options options;
    auto next = [&](int& index, const char* name) -> std::string {
        if (++index >= argc)
            throw std::runtime_error(std::string("Missing value after ") + name);
        return argv[index];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--host")
            options.host = next(i, "--host");
        else if (argument == "--port")
            options.port = uint16_t(integer(next(i, "--port"), "port", 65535));
        else if (argument == "--size") {
            auto size = uint16_t(integer(next(i, "--size"), "size", 1024));
            options.width = options.height = size;
        } else if (argument == "--width")
            options.width = uint16_t(integer(next(i, "--width"), "width", 1024));
        else if (argument == "--height")
            options.height = uint16_t(integer(next(i, "--height"), "height", 1024));
        else if (argument == "--output")
            options.output = integer(next(i, "--output"), "output", 1024);
        else if (argument == "--capture") {
            const auto name = next(i, "--capture");
            const auto method = frame_sender::parse_capture_method(name);
            if (!method)
                throw std::runtime_error("Unknown capture method: " + name + "; choose dxgi, wgc, or gdi");
            options.capture_method = *method;
        } else if (argument == "--fps")
            options.fps = real(next(i, "--fps"), "fps", 1000);
        else if (argument == "--send-buffer-mib")
            options.send_buffer_mib = int(integer(next(i, "--send-buffer-mib"), "send buffer", 16));
        else if (argument == "--sync-resolution")
            options.sync_resolution = true;
        else if (argument == "--no-sync-resolution")
            options.sync_resolution = false;
        else if (argument == "--list-outputs")
            options.list_outputs = true;
        else if (argument == "--list-capture-methods")
            options.list_capture_methods = true;
        else if (argument == "--self-test")
            options.self_test = true;
        else if (argument == "--version") {
            std::cout << "FrameSender " FRAME_SENDER_VERSION "\n";
            std::exit(0);
        } else if (argument == "--help" || argument == "-h") {
            usage();
            std::exit(0);
        } else
            throw std::runtime_error("Unknown option: " + argument);
    }
    if (options.host.empty() || !options.port || !options.width || !options.height ||
        !options.send_buffer_mib)
        throw std::runtime_error("Host, port, crop size, and send buffer must be nonzero");
    return options;
}

struct OutputChoice {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description{};
};

std::vector<OutputChoice> outputs() {
    ComPtr<IDXGIFactory1> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    std::vector<OutputChoice> result;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        checked(hr, "EnumAdapters1");
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            checked(hr, "EnumOutputs");
            OutputChoice choice{adapter, output, {}};
            checked(output->GetDesc(&choice.description), "IDXGIOutput::GetDesc");
            result.push_back(std::move(choice));
        }
    }
    return result;
}

void list_outputs() {
    auto available = outputs();
    if (available.empty()) {
        std::cout << "No attached desktop outputs were found.\n";
        return;
    }
    for (size_t i = 0; i < available.size(); ++i) {
        const auto& d = available[i].description;
        std::cout << i << ": " << utf8(d.DeviceName) << "  "
                  << d.DesktopCoordinates.right - d.DesktopCoordinates.left << 'x'
                  << d.DesktopCoordinates.bottom - d.DesktopCoordinates.top
                  << (d.AttachedToDesktop ? "" : " (detached)") << '\n';
    }
}

struct CapturedFrame {
    uint16_t width = 0;
    uint16_t height = 0;
    int64_t capture_ns = 0;
    std::vector<uint8_t> pixels;
};

class Capture {
  protected:
    OutputChoice choice_;
    RECT desktop_bounds_{};
    uint64_t display_id_ = 1469598103934665603ull;

    explicit Capture(unsigned output_index) {
        auto available = outputs();
        if (output_index >= available.size())
            throw std::runtime_error("Output index is unavailable; run --list-outputs");
        choice_ = std::move(available[output_index]);
        if (!choice_.description.AttachedToDesktop)
            throw std::runtime_error("Selected output is not attached to the desktop");
        desktop_bounds_ = choice_.description.DesktopCoordinates;
        for (const auto ch : std::wstring(choice_.description.DeviceName)) {
            display_id_ ^= uint16_t(ch);
            display_id_ *= 1099511628211ull;
        }
        std::cout << "Capturing output " << output_index << " (" << utf8(choice_.description.DeviceName)
                  << ", " << desktop_bounds_.right - desktop_bounds_.left << 'x'
                  << desktop_bounds_.bottom - desktop_bounds_.top << ")\n";
    }

    void check_monitor() const {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(choice_.description.Monitor, &info) ||
            !EqualRect(&info.rcMonitor, &desktop_bounds_))
            throw HrError(DXGI_ERROR_ACCESS_LOST, "Selected display geometry changed");
    }

  public:
    virtual ~Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    cursor_protocol::Sample geometry() const {
        cursor_protocol::Sample s;
        s.left = desktop_bounds_.left;
        s.top = desktop_bounds_.top;
        s.width = desktop_bounds_.right - desktop_bounds_.left;
        s.height = desktop_bounds_.bottom - desktop_bounds_.top;
        s.display = display_id_;
        s.generation = 1;
        return s;
    }

    bool fits(uint16_t width, uint16_t height) const {
        const auto s = geometry();
        return frame_sender::center_crop(s.width, s.height, width, height).has_value();
    }

    virtual bool next(CapturedFrame& frame, uint16_t width, uint16_t height) = 0;
};

class GpuCapture : public Capture {
  protected:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    uint16_t staging_width_ = 0;
    uint16_t staging_height_ = 0;

    explicit GpuCapture(unsigned output_index) : Capture(output_index) {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        checked(D3D11CreateDevice(choice_.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, UINT(std::size(levels)),
                                  D3D11_SDK_VERSION, &device_, &selected, &context_),
                "D3D11CreateDevice");
    }

    void make_staging(uint16_t width, uint16_t height) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        checked(device_->CreateTexture2D(&description, nullptr, &staging_),
                "Create center-crop staging texture");
        staging_width_ = width;
        staging_height_ = height;
    }

    bool read_crop(ID3D11Texture2D* desktop, CapturedFrame& frame, uint16_t width,
                   uint16_t height, int64_t capture_ns) {
        D3D11_TEXTURE2D_DESC source{};
        desktop->GetDesc(&source);
        const auto crop = frame_sender::center_crop(source.Width, source.Height, width, height);
        if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !crop)
            throw std::runtime_error("Desktop surface changed to an unsupported shape or format");
        if (!staging_ || width != staging_width_ || height != staging_height_)
            make_staging(width, height);

        const D3D11_BOX box{crop->left, crop->top, 0, crop->left + width, crop->top + height, 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map center crop");
        struct Unmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~Unmap() { context->Unmap(texture, 0); }
        } unmap{context_.Get(), staging_.Get()};
        frame.width = width;
        frame.height = height;
        frame.capture_ns = capture_ns;
        frame.pixels.resize(size_t(width) * height * 4);
        const size_t row_bytes = size_t(width) * 4;
        for (uint16_t y = 0; y < height; ++y)
            std::memcpy(frame.pixels.data() + size_t(y) * row_bytes,
                        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, row_bytes);
        return true;
    }
};

class DesktopCapture : public GpuCapture {
    ComPtr<IDXGIOutputDuplication> duplication_;
    bool protected_content_reported_ = false;

  public:
    explicit DesktopCapture(unsigned output_index) : GpuCapture(output_index) {
        ComPtr<IDXGIOutput1> output1;
        checked(choice_.output.As(&output1), "Query IDXGIOutput1");
        checked(output1->DuplicateOutput(device_.Get(), &duplication_), "DuplicateOutput");
        DXGI_OUTDUPL_DESC description{};
        duplication_->GetDesc(&description);
        if (description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Desktop output is not available as lossless BGRA8");
        if (description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
            description.Rotation != DXGI_MODE_ROTATION_IDENTITY)
            throw std::runtime_error(
                "Rotated outputs are not supported; select a landscape output with no rotation");
    }

    bool next(CapturedFrame& frame, uint16_t width, uint16_t height) override {
        check_monitor();
        if (!fits(width, height))
            throw std::runtime_error("Requested crop is larger than the selected output");
        DXGI_OUTDUPL_FRAME_INFO information{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(16, &information, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return false;
        checked(hr, "AcquireNextFrame");
        struct Release {
            IDXGIOutputDuplication* value;
            ~Release() {
                value->ReleaseFrame();
            }
        } release{duplication_.Get()};

        if (information.ProtectedContentMaskedOut && !protected_content_reported_) {
            std::cerr
                << "Windows masked protected content in the duplicated desktop; it will remain masked.\n";
            protected_content_reported_ = true;
        }

        // Pointer-only updates do not change the captured pixels.
        if (!information.LastPresentTime.QuadPart)
            return false;
        ComPtr<ID3D11Texture2D> desktop;
        checked(resource.As(&desktop), "Query desktop texture");
        return read_crop(desktop.Get(), frame, width, height,
                         qpc_to_ns(information.LastPresentTime.QuadPart));
    }
};

class GraphicsCapture : public GpuCapture {
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    } apartment_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    std::shared_ptr<std::atomic<bool>> closed_ = std::make_shared<std::atomic<bool>>(false);
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closed_revoker_;
    bool started_ = false;

  public:
    explicit GraphicsCapture(unsigned output_index) try : GpuCapture(output_index) {
        using namespace winrt::Windows::Graphics::Capture;
        if (!GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows Graphics Capture is unavailable on this system; try --capture dxgi or gdi");
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(choice_.description.Monitor,
            winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));

        ComPtr<IDXGIDevice> dxgi_device;
        checked(device_.As(&dxgi_device), "Query WGC DXGI device");
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()));
        winrt_device_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_device_,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item_.Size());
        session_ = pool_.CreateCaptureSession(item_);
        if (!session_.try_as<IGraphicsCaptureSession2>())
            throw std::runtime_error("Cursor-free WGC requires Windows 10 2004 or newer; try --capture dxgi or gdi");
        session_.IsCursorCaptureEnabled(false);
        // Keep the system capture indicator and all Windows content protections.
        closed_revoker_ = item_.Closed(winrt::auto_revoke,
            [closed = closed_](const auto&, const auto&) { closed->store(true); });
    } catch (const winrt::hresult_error& error) {
        throw HrError(error.code(), "Initialize Windows Graphics Capture (requires Windows 10 2004+)");
    }

    ~GraphicsCapture() override {
        // Stop the producer before releasing its pool, textures and apartment.
        closed_revoker_.revoke();
        try { if (session_) session_.Close(); } catch (...) {}
        try { if (pool_) pool_.Close(); } catch (...) {}
    }

    bool next(CapturedFrame& frame, uint16_t width, uint16_t height) override {
        try {
            check_monitor();
            if (!fits(width, height))
                throw std::runtime_error("Requested crop is larger than the selected output");
            // Waiting for Receiver must not fill the pool with old startup frames.
            if (!started_) {
                session_.StartCapture();
                started_ = true;
            }
            const int64_t deadline = now_ns() + 16'000'000;
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest{nullptr};
            do {
                if (closed_->load())
                    throw HrError(DXGI_ERROR_ACCESS_LOST, "WGC display closed");
                latest = pool_.TryGetNextFrame();
                if (latest)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (now_ns() < deadline);
            if (!latest)
                return false;
            // Drain only the bounded pool, so a fast producer cannot starve sending.
            if (auto newer = pool_.TryGetNextFrame()) {
                latest.Close();
                latest = std::move(newer);
            }
            struct CloseFrame {
                winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& value;
                ~CloseFrame() { try { value.Close(); } catch (...) {} }
            } close{latest};
            const auto size = latest.ContentSize();
            const auto g = geometry();
            if (size.Width != g.width || size.Height != g.height)
                throw HrError(DXGI_ERROR_ACCESS_LOST, "WGC display size changed");
            auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            // SystemRelativeTime is QPC-based, in 100 ns units. The compositor can
            // predict a future presentation; never report capture after acquisition.
            const int64_t stamp = std::min<int64_t>(latest.SystemRelativeTime().count() * 100, now_ns());
            return read_crop(texture.Get(), frame, width, height, stamp);
        } catch (const winrt::hresult_error& error) {
            throw HrError(error.code(), "Acquire Windows Graphics Capture frame");
        }
    }
};

class GdiCapture : public Capture {
    struct ScreenDc {
        HDC value = nullptr;
        ~ScreenDc() { if (value) ReleaseDC(nullptr, value); }
    } screen_;
    struct MemoryDc {
        HDC value = nullptr;
        ~MemoryDc() { if (value) DeleteDC(value); }
    } memory_;
    struct Dib {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* pixels = nullptr;
        uint16_t width = 0, height = 0;
        void reset() {
            if (previous) SelectObject(dc, previous);
            if (bitmap) DeleteObject(bitmap);
            previous = nullptr;
            bitmap = nullptr;
            pixels = nullptr;
            width = height = 0;
        }
        ~Dib() { reset(); }
    } dib_;
    int64_t interval_ns_;
    int64_t next_capture_ns_ = 0;

    void make_dib(uint16_t width, uint16_t height) {
        dib_.reset();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -LONG(height); // Top-down rows match DXGI and UVF1.
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dib_.bitmap = CreateDIBSection(screen_.value, &info, DIB_RGB_COLORS, &dib_.pixels, nullptr, 0);
        if (!dib_.bitmap)
            throw std::runtime_error("CreateDIBSection failed for GDI center crop");
        dib_.dc = memory_.value;
        const auto previous = SelectObject(dib_.dc, dib_.bitmap);
        if (!previous || previous == HGDI_ERROR)
            throw std::runtime_error("SelectObject failed for GDI center crop");
        dib_.previous = previous;
        dib_.width = width;
        dib_.height = height;
    }

  public:
    GdiCapture(unsigned output_index, double fps) : Capture(output_index),
        interval_ns_(int64_t(1e9 / (fps > 0 ? fps : 60))) {
        screen_.value = GetDC(nullptr);
        if (!screen_.value)
            throw std::runtime_error("Could not acquire the desktop DC for GDI capture");
        memory_.value = CreateCompatibleDC(screen_.value);
        if (!memory_.value)
            throw std::runtime_error("CreateCompatibleDC failed for GDI capture");
    }

    bool next(CapturedFrame& frame, uint16_t width, uint16_t height) override {
        check_monitor();
        const auto g = geometry();
        const auto crop = frame_sender::center_crop(g.width, g.height, width, height);
        if (!crop)
            throw std::runtime_error("Requested crop is larger than the selected output");
        const int64_t now = now_ns();
        if (now < next_capture_ns_) {
            // Return at least every 16 ms for Ctrl+C and Receiver size updates.
            std::this_thread::sleep_for(std::chrono::nanoseconds(
                std::min<int64_t>(16'000'000, next_capture_ns_ - now)));
            return false;
        }
        if (dib_.width != width || dib_.height != height)
            make_dib(width, height);
        // Capture physical screen coordinates, including monitors left/above primary.
        if (!BitBlt(memory_.value, 0, 0, width, height, screen_.value,
                    g.left + int(crop->left), g.top + int(crop->top), SRCCOPY | CAPTUREBLT) || !GdiFlush())
            throw std::runtime_error("GDI desktop capture failed; check the interactive desktop session");
        frame.width = width;
        frame.height = height;
        frame.capture_ns = now_ns(); // GDI supplies no presentation timestamp.
        next_capture_ns_ = frame.capture_ns + interval_ns_;
        frame.pixels.resize(size_t(width) * height * 4);
        std::memcpy(frame.pixels.data(), dib_.pixels, frame.pixels.size());
        // BI_RGB's fourth byte is undefined; publish an opaque BGRA alpha channel.
        for (size_t i = 3; i < frame.pixels.size(); i += 4)
            frame.pixels[i] = 255;
        return true;
    }
};

std::unique_ptr<Capture> make_capture(const Options& options) {
    using frame_sender::CaptureMethod;
    switch (options.capture_method) {
    case CaptureMethod::dxgi: return std::make_unique<DesktopCapture>(options.output);
    case CaptureMethod::wgc: return std::make_unique<GraphicsCapture>(options.output);
    case CaptureMethod::gdi: return std::make_unique<GdiCapture>(options.output, options.fps);
    }
    throw std::runtime_error("Invalid capture method");
}

class Network {
    struct Winsock {
        Winsock() {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data))
                throw std::runtime_error("WSAStartup failed");
        }
        ~Winsock() {
            WSACleanup();
        }
    } winsock_;

  public:
    SOCKET socket = INVALID_SOCKET;

    Network(const std::string& host, uint16_t port, int send_buffer_mib) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* addresses = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) || !addresses)
            throw std::runtime_error("Could not resolve Receiver IPv4 address: " + host);
        struct FreeAddress {
            addrinfo* value;
            ~FreeAddress() {
                freeaddrinfo(value);
            }
        } free_address{addresses};
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET)
            throw std::runtime_error("UDP socket creation failed");
        int send_buffer = send_buffer_mib * 1024 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_buffer),
                   sizeof(send_buffer));
        BOOL reset_on_unreachable = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket, SIO_UDP_CONNRESET, &reset_on_unreachable, sizeof(reset_on_unreachable), nullptr, 0,
                 &returned, nullptr, nullptr);
        if (connect(socket, addresses->ai_addr, int(addresses->ai_addrlen)) == SOCKET_ERROR) {
            closesocket(socket);
            socket = INVALID_SOCKET;
            throw std::runtime_error("Could not connect UDP socket to Receiver");
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(socket);
            socket = INVALID_SOCKET;
            throw std::runtime_error("Could not make UDP socket nonblocking");
        }
    }

    ~Network() {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
    }

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

bool transient_socket_error(int error) {
    return error == WSAEWOULDBLOCK || error == WSAENOBUFS || error == WSAECONNRESET ||
           error == WSAENETUNREACH || error == WSAEHOSTUNREACH;
}

bool send_control(SOCKET socket, const uint8_t* data, size_t size) {
    int sent = send(socket, reinterpret_cast<const char*>(data), int(size), 0);
    if (sent == int(size))
        return true;
    if (sent == SOCKET_ERROR && transient_socket_error(WSAGetLastError()))
        return false;
    throw std::runtime_error("UDP control send failed");
}

struct SendResult {
    bool complete = false;
    uint32_t datagrams = 0;
    uint64_t bytes = 0;
};

std::array<uint8_t, frame_header_size> encode_frame_header(const CapturedFrame& frame, uint64_t session,
                                                           uint32_t sequence, uint16_t index, uint16_t count,
                                                           uint32_t offset) {
    std::array<uint8_t, frame_header_size> header{};
    std::memcpy(header.data(), "UVF1", 4);
    header[4] = 1;
    header[5] = 2; // Exact desktop BGRA8; no conversion or compression.
    put16(header.data() + 6, uint16_t(frame_header_size));
    put64(header.data() + 8, session);
    put32(header.data() + 16, sequence);
    put64(header.data() + 20, uint64_t(frame.capture_ns));
    put16(header.data() + 28, frame.width);
    put16(header.data() + 30, frame.height);
    put32(header.data() + 32, uint32_t(frame.pixels.size()));
    put16(header.data() + 36, index);
    put16(header.data() + 38, count);
    put32(header.data() + 40, offset);
    put16(header.data() + 44, frame_stride);
    return header;
}

SendResult send_frame(SOCKET socket, const CapturedFrame& frame, uint64_t session, uint32_t sequence) {
    const uint32_t bytes = uint32_t(frame.pixels.size());
    const uint16_t count = uint16_t((bytes + frame_stride - 1) / frame_stride);
    SendResult result;
    for (uint16_t index = 0; index < count; ++index) {
        const uint32_t offset = uint32_t(index) * frame_stride;
        const uint32_t payload = std::min<uint32_t>(frame_stride, bytes - offset);
        auto header = encode_frame_header(frame, session, sequence, index, count, offset);
        WSABUF buffers[2] = {
            {ULONG(header.size()), reinterpret_cast<char*>(header.data())},
            {ULONG(payload), reinterpret_cast<char*>(const_cast<uint8_t*>(frame.pixels.data() + offset))}};
        DWORD sent = 0;
        if (WSASend(socket, buffers, 2, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (transient_socket_error(error))
                return result; // Abandon this frame; never wait behind stale pixels.
            throw std::runtime_error("UDP frame send failed with Winsock error " + std::to_string(error));
        }
        if (sent != frame_header_size + payload)
            return result;
        ++result.datagrams;
        result.bytes += sent;
    }
    result.complete = true;
    return result;
}

void self_test() {
    char executable[] = "frame_sender";
    char manual[] = "--no-sync-resolution";
    char automatic[] = "--sync-resolution";
    char* arguments[] = {executable, manual, automatic};
    if (!parse_options(1, arguments).sync_resolution || parse_options(2, arguments).sync_resolution ||
        !parse_options(3, arguments).sync_resolution)
        throw std::runtime_error("Resolution sync option self-test failed");
    constexpr uint64_t session = 0x0102030405060708ull;
    CapturedFrame frame;
    frame.width = 320;
    frame.height = 160;
    frame.capture_ns = 0x0011223344556677ll;
    frame.pixels.resize(size_t(frame.width) * frame.height * 4);
    const uint16_t count = uint16_t((frame.pixels.size() + frame_stride - 1) / frame_stride);
    auto header = encode_frame_header(frame, session, 0x89abcdefu, uint16_t(count - 1), count,
                                      uint32_t(count - 1) * frame_stride);
    const bool valid =
        !std::memcmp(header.data(), "UVF1", 4) && header[4] == 1 && header[5] == 2 &&
        get16(header.data() + 6) == frame_header_size && get64(header.data() + 8) == session &&
        get32(header.data() + 16) == 0x89abcdefu && get64(header.data() + 20) == uint64_t(frame.capture_ns) &&
        get16(header.data() + 28) == frame.width && get16(header.data() + 30) == frame.height &&
        get32(header.data() + 32) == frame.pixels.size() && get16(header.data() + 36) == count - 1 &&
        get16(header.data() + 38) == count &&
        get32(header.data() + 40) == uint32_t(count - 1) * frame_stride &&
        get16(header.data() + 44) == frame_stride && get16(header.data() + 46) == 0;
    if (!valid)
        throw std::runtime_error("UVF1 wire self-test failed");
    std::cout << "FrameSender wire self-test passed (" << count << " fragments for 320x160 BGRA).\n";
}

class ControlPlane {
    SOCKET socket_;
    uint64_t session_;
    bool accept_resolution_;
    std::atomic<uint32_t> desired_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::mutex cursor_mutex_;
    cursor_protocol::Sample cursor_;
    uint32_t cursor_sequence_ = 0;
    std::thread thread_;

    static uint32_t pack(uint16_t width, uint16_t height) {
        return uint32_t(width) << 16 | height;
    }

    void run() {
        int64_t next_hello = 0;
        try {
            while (!stop_.load(std::memory_order_relaxed)) {
                const int64_t now = now_ns();
                if (now >= next_hello) {
                    std::array<uint8_t, 16> hello{};
                    std::memcpy(hello.data(), "UVH1", 4);
                    put64(hello.data() + 8, session_);
                    send_control(socket_, hello.data(), hello.size());
                    next_hello = now + 50'000'000;
                }
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(socket_, &readable);
                timeval timeout{0, 5'000};
                int ready = select(0, &readable, nullptr, nullptr, &timeout);
                if (ready == SOCKET_ERROR)
                    throw std::runtime_error("UDP control receive wait failed");
                if (!ready)
                    continue;
                std::array<uint8_t, 256> packet{};
                const int length =
                    recv(socket_, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0);
                const int64_t received = now_ns();
                if (length == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (transient_socket_error(error) || error == WSAEMSGSIZE)
                        continue;
                    throw std::runtime_error("UDP control receive failed");
                }
                if (length == 24 && !std::memcmp(packet.data(), "UVC1", 4) && get32(packet.data() + 4) == 0 &&
                    get64(packet.data() + 8) == session_) {
                    std::array<uint8_t, 40> reply{};
                    std::memcpy(reply.data(), "UVS1", 4);
                    put64(reply.data() + 8, session_);
                    put64(reply.data() + 16, get64(packet.data() + 16));
                    put64(reply.data() + 24, uint64_t(received));
                    put64(reply.data() + 32, uint64_t(now_ns()));
                    send_control(socket_, reply.data(), reply.size());
                } else if (length == 24 && !std::memcmp(packet.data(), "UVP1", 4)) {
                    uint64_t token = 0;
                    if (cursor_protocol::parse_request(packet.data(), size_t(length), session_, token)) {
                        cursor_protocol::Sample sample;
                        {
                            std::lock_guard<std::mutex> lock(cursor_mutex_);
                            sample = cursor_;
                        }
                        sample.session = session_;
                        sample.token = token;
                        sample.sequence = ++cursor_sequence_;
                        POINT position{};
                        CURSORINFO info{};
                        info.cbSize = sizeof(info);
                        if (GetCursorPos(&position)) {
                            sample.flags |= 1;
                            sample.x = position.x;
                            sample.y = position.y;
                        }
                        if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING))
                            sample.flags |= 2;
                        sample.sample_ns = uint64_t(now_ns());
                        auto response = cursor_protocol::packet(sample);
                        send_control(socket_, response.data(), response.size());
                    }
                } else if (accept_resolution_ && length > 0) {
                    auto resolution = frame_sender::parse_resolution_request(
                        std::span<const uint8_t>(packet.data(), size_t(length)), session_);
                    if (resolution)
                        desired_.store(pack(resolution->width, resolution->height),
                                       std::memory_order_release);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Control channel stopped: " << error.what() << '\n';
            failed_.store(true, std::memory_order_relaxed);
            stop_.store(true, std::memory_order_relaxed);
        }
    }

  public:
    ControlPlane(SOCKET socket, uint64_t session, uint16_t width, uint16_t height, bool accept_resolution,
                 cursor_protocol::Sample geometry)
        : socket_(socket), session_(session), accept_resolution_(accept_resolution),
          desired_(accept_resolution ? 0 : pack(width, height)), cursor_(geometry),
          thread_([this] { run(); }) {}

    ~ControlPlane() {
        stop();
    }

    void stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
    }

    void publish(cursor_protocol::Sample sample, uint16_t width, uint16_t height, uint32_t sequence,
                 int64_t stamp) {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        const bool changed = cursor_.display != sample.display || cursor_.left != sample.left ||
                             cursor_.top != sample.top || cursor_.width != sample.width ||
                             cursor_.height != sample.height || cursor_.crop_width != width ||
                             cursor_.crop_height != height;
        sample.generation = cursor_.generation + (changed ? 1 : 0);
        if (!sample.generation)
            sample.generation = 1;
        sample.crop_width = width;
        sample.crop_height = height;
        sample.crop_left = sample.left + (sample.width - width) / 2;
        sample.crop_top = sample.top + (sample.height - height) / 2;
        sample.flags = width && height && stamp > 0 ? 4 : 0;
        sample.frame_sequence = sequence;
        sample.frame_ns = uint64_t(stamp);
        cursor_ = sample;
    }

    bool stopped() const {
        return stop_.load(std::memory_order_relaxed);
    }

    bool failed() const {
        return failed_.load(std::memory_order_relaxed);
    }

    std::pair<uint16_t, uint16_t> desired() const {
        uint32_t value = desired_.load(std::memory_order_acquire);
        return {uint16_t(value >> 16), uint16_t(value)};
    }
};

std::atomic<bool>* console_stop = nullptr;

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        if (console_stop)
            console_stop->store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

class ConsoleHandlerGuard {
  public:
    explicit ConsoleHandlerGuard(std::atomic<bool>& stop) {
        console_stop = &stop;
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            console_stop = nullptr;
            throw std::runtime_error("Could not install Ctrl+C handler");
        }
    }
    ~ConsoleHandlerGuard() {
        SetConsoleCtrlHandler(console_handler, FALSE);
        console_stop = nullptr;
    }
};
} // namespace

frame_sender::Options frame_sender::parse_arguments(int argc, char** argv) {
    return parse_options(argc, argv);
}

std::vector<frame_sender::DisplayInfo> frame_sender::display_choices() {
    const auto available = outputs();
    std::vector<DisplayInfo> result;
    for (unsigned i = 0; i < available.size(); ++i) {
        const auto& d = available[i].description;
        if (d.AttachedToDesktop)
            result.push_back({i, std::to_wstring(i) + L": " + d.DeviceName + L"  " +
                std::to_wstring(d.DesktopCoordinates.right - d.DesktopCoordinates.left) + L" x " +
                std::to_wstring(d.DesktopCoordinates.bottom - d.DesktopCoordinates.top)});
    }
    return result;
}

int frame_sender::run_cli(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const Options options = parse_options(argc, argv);
        if (options.list_capture_methods) {
            for (const auto& info : frame_sender::capture_methods)
                std::cout << info.name << ": " << info.description << '\n';
            return 0;
        }
        if (options.list_outputs) {
            list_outputs();
            return 0;
        }
        if (options.self_test) {
            self_test();
            return 0;
        }
        std::atomic<bool> stop{false};
        ConsoleHandlerGuard console(stop);
        std::cout << "Press Ctrl+C to stop.\n";
        return run_sender(options, stop);
    } catch (const std::exception& error) {
        std::cerr << "frame_sender: " << error.what() << '\n';
        return 1;
    }
}

int frame_sender::run_sender(const Options& options, std::atomic<bool>& stop) {
    try {
        Network network(options.host, options.port, options.send_buffer_mib);
        const uint64_t session = random_session();
        for (const auto& info : frame_sender::capture_methods)
            if (info.method == options.capture_method)
                std::cout << "Capture method: " << info.name << " - " << info.description << '\n';
        auto capture = make_capture(options);
        uint16_t width = options.sync_resolution ? 0 : options.width;
        uint16_t height = options.sync_resolution ? 0 : options.height;
        if (!options.sync_resolution && !capture->fits(width, height))
            throw std::runtime_error("Initial crop is larger than the selected output");
        ControlPlane control(network.socket, session, options.width, options.height, options.sync_resolution,
                             capture->geometry());
        if (options.sync_resolution)
            std::cout << "Waiting for Receiver's capture size at " << options.host << ':' << options.port
                      << ". Start Receiver with this PC's Sender IPv4 configured.\n";
        else
            std::cout << "Sending fixed center crop " << width << 'x' << height << " BGRA to " << options.host
                      << ':' << options.port << "\n";

        CapturedFrame frame;
        uint32_t sequence = 0;
        uint64_t captured = 0, sent_frames = 0, dropped = 0, skipped = 0, datagrams = 0, wire_bytes = 0;
        uint64_t previous_captured = 0, previous_sent = 0, previous_bytes = 0;
        int64_t report_at = now_ns() + 1'000'000'000;
        int64_t next_send = 0, backpressure_until = 0;
        const int64_t interval = options.fps > 0 ? int64_t(1e9 / options.fps) : 0;

        while (!stop.load(std::memory_order_relaxed) && !control.stopped()) {
            auto [requested_width, requested_height] = control.desired();
            if (!requested_width || !requested_height) {
                // Heartbeats and clock replies continue while the model loads. Do
                // not send guessed dimensions before Receiver publishes its size.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (requested_width != width || requested_height != height) {
                if (!capture->fits(requested_width, requested_height))
                    throw std::runtime_error("Receiver requested " + std::to_string(requested_width) + "x" +
                                             std::to_string(requested_height) +
                                             ", which is larger than the selected output");
                width = requested_width;
                height = requested_height;
                next_send = backpressure_until = 0;
                std::cout << "Receiver requested exact center crop " << width << 'x' << height << "\n";
            }
            try {
                if (!capture->next(frame, width, height))
                    continue;
            } catch (const HrError& error) {
                if (error.value != DXGI_ERROR_ACCESS_LOST)
                    throw;
                std::cerr << "Display changed; recreating desktop capture.\n";
                capture.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                capture = make_capture(options);
                if (width && height && !capture->fits(width, height))
                    throw std::runtime_error("Crop is larger than the reconfigured output");
                control.publish(capture->geometry(), 0, 0, 0, 0);
                continue;
            }
            ++captured;
            const uint32_t frame_sequence = sequence++;
            if (control.desired() != std::pair{width, height}) {
                ++skipped;
                continue; // The request changed while capture was acquiring this frame.
            }
            const int64_t now = now_ns();
            if ((interval && now < next_send) || now < backpressure_until) {
                ++skipped;
                continue;
            }
            next_send = interval ? now + interval : 0;
            const SendResult result = send_frame(network.socket, frame, session, frame_sequence);
            datagrams += result.datagrams;
            wire_bytes += result.bytes;
            if (result.complete) {
                ++sent_frames;
                control.publish(capture->geometry(), width, height, frame_sequence, frame.capture_ns);
            } else {
                ++dropped;
                // Let already-queued fragments drain before attempting another complete frame.
                // One bit/ns is a conservative 1 Gbit/s recovery estimate; faster links pay this
                // cost only after actual backpressure.
                const int64_t recovery = std::max<int64_t>(1'000'000, int64_t(frame.pixels.size() * 8));
                backpressure_until = now + recovery;
            }

            if (now >= report_at) {
                std::cout << "capture " << (captured - previous_captured) << " fps, sent "
                          << (sent_frames - previous_sent) << " fps, " << skipped << " paced/skipped, "
                          << dropped << " backpressure drops, " << std::fixed << std::setprecision(1)
                          << double(wire_bytes - previous_bytes) * 8 / 1e6 << " Mbit/s, " << width << 'x'
                          << height << '\n';
                previous_captured = captured;
                previous_sent = sent_frames;
                previous_bytes = wire_bytes;
                report_at = now + 1'000'000'000;
            }
        }
        stop.store(true, std::memory_order_relaxed);
        control.stop();
        std::cout << "Stopped. Captured " << captured << ", sent " << sent_frames << ", dropped " << dropped
                  << ", paced/skipped " << skipped << ", datagrams " << datagrams << ".\n";
        return control.failed() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "frame_sender: " << error.what() << '\n';
        return 1;
    }
}
