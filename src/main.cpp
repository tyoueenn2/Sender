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

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 5000;
    uint16_t width = 320;
    uint16_t height = 320;
    unsigned output = 0;
    double fps = 0;
    int send_buffer_mib = 2;
    bool sync_resolution = false;
    bool list_outputs = false;
    bool self_test = false;
};

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
    std::cout
        << "FrameSender " FRAME_SENDER_VERSION
           " - low-latency, center-crop-only Windows frame sender\n\n"
        << "frame_sender --host RECEIVER_IPV4 [options]\n\n"
        << "  --size PIXELS           square crop (default 320)\n"
        << "  --width PIXELS          crop width, 1..1024\n"
        << "  --height PIXELS         crop height, 1..1024\n"
        << "  --sync-resolution       follow the Receiver model size\n"
        << "  --output INDEX          desktop output from --list-outputs\n"
        << "  --fps FPS               cap sending; 0 sends every new present\n"
        << "  --port PORT             Receiver frame port (default 5000)\n"
        << "  --send-buffer-mib MIB   UDP queue, 1..16 (default 2)\n"
        << "  --list-outputs          list displays and exit\n"
        << "  --self-test             verify wire encoding and exit\n"
        << "  --version               show the software version\n"
        << "  --help                  show this text\n";
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
        else if (argument == "--fps")
            options.fps = real(next(i, "--fps"), "fps", 1000);
        else if (argument == "--send-buffer-mib")
            options.send_buffer_mib = int(integer(next(i, "--send-buffer-mib"), "send buffer", 16));
        else if (argument == "--sync-resolution")
            options.sync_resolution = true;
        else if (argument == "--list-outputs")
            options.list_outputs = true;
        else if (argument == "--self-test")
            options.self_test = true;
        else if (argument == "--version") {
            std::cout << "FrameSender " FRAME_SENDER_VERSION "\n";
            std::exit(0);
        }
        else if (argument == "--help" || argument == "-h") {
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

class DesktopCapture {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    DXGI_OUTDUPL_DESC duplication_description_{};
    uint16_t staging_width_ = 0;
    uint16_t staging_height_ = 0;
    bool protected_content_reported_ = false;

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

  public:
    explicit DesktopCapture(unsigned output_index) {
        auto available = outputs();
        if (output_index >= available.size())
            throw std::runtime_error("Output index is unavailable; run --list-outputs");
        const auto& choice = available[output_index];
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        checked(D3D11CreateDevice(choice.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                                  levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, &selected,
                                  &context_),
                "D3D11CreateDevice");
        ComPtr<IDXGIOutput1> output1;
        checked(choice.output.As(&output1), "Query IDXGIOutput1");
        checked(output1->DuplicateOutput(device_.Get(), &duplication_), "DuplicateOutput");
        duplication_->GetDesc(&duplication_description_);
        if (duplication_description_.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Desktop output is not available as lossless BGRA8");
        if (duplication_description_.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
            duplication_description_.Rotation != DXGI_MODE_ROTATION_IDENTITY)
            throw std::runtime_error(
                "Rotated outputs are not supported; select a landscape output with no rotation");
        std::cout << "Capturing output " << output_index << " (" << utf8(choice.description.DeviceName)
                  << ", " << duplication_description_.ModeDesc.Width << 'x'
                  << duplication_description_.ModeDesc.Height << ")\n";
    }

    bool fits(uint16_t width, uint16_t height) const {
        return width <= duplication_description_.ModeDesc.Width &&
               height <= duplication_description_.ModeDesc.Height;
    }

    bool next(CapturedFrame& frame, uint16_t width, uint16_t height, UINT timeout_ms = 16) {
        if (!fits(width, height))
            throw std::runtime_error("Requested crop is larger than the selected output");
        DXGI_OUTDUPL_FRAME_INFO information{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &information, &resource);
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
            std::cerr << "Windows masked protected content in the duplicated desktop; it will remain masked.\n";
            protected_content_reported_ = true;
        }

        // Pointer-only updates do not change the captured pixels.
        if (!information.LastPresentTime.QuadPart)
            return false;
        ComPtr<ID3D11Texture2D> desktop;
        checked(resource.As(&desktop), "Query desktop texture");
        D3D11_TEXTURE2D_DESC source{};
        desktop->GetDesc(&source);
        if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || width > source.Width || height > source.Height)
            throw std::runtime_error("Desktop surface changed to an unsupported shape or format");
        if (!staging_ || width != staging_width_ || height != staging_height_)
            make_staging(width, height);

        const UINT left = (source.Width - width) / 2;
        const UINT top = (source.Height - height) / 2;
        const D3D11_BOX crop{left, top, 0, left + width, top + height, 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop.Get(), 0, &crop);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map center crop");
        struct Unmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~Unmap() {
                context->Unmap(texture, 0);
            }
        } unmap{context_.Get(), staging_.Get()};
        frame.width = width;
        frame.height = height;
        frame.capture_ns = qpc_to_ns(information.LastPresentTime.QuadPart);
        frame.pixels.resize(size_t(width) * height * 4);
        const size_t row_bytes = size_t(width) * 4;
        for (uint16_t y = 0; y < height; ++y)
            std::memcpy(frame.pixels.data() + size_t(y) * row_bytes,
                        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        row_bytes);
        return true;
    }
};

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
        WSAIoctl(socket, SIO_UDP_CONNRESET, &reset_on_unreachable, sizeof(reset_on_unreachable),
                 nullptr, 0, &returned, nullptr, nullptr);
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

std::array<uint8_t, frame_header_size> encode_frame_header(const CapturedFrame& frame,
                                                           uint64_t session, uint32_t sequence,
                                                           uint16_t index, uint16_t count,
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

SendResult send_frame(SOCKET socket, const CapturedFrame& frame, uint64_t session,
                      uint32_t sequence) {
    const uint32_t bytes = uint32_t(frame.pixels.size());
    const uint16_t count = uint16_t((bytes + frame_stride - 1) / frame_stride);
    SendResult result;
    for (uint16_t index = 0; index < count; ++index) {
        const uint32_t offset = uint32_t(index) * frame_stride;
        const uint32_t payload = std::min<uint32_t>(frame_stride, bytes - offset);
        auto header = encode_frame_header(frame, session, sequence, index, count, offset);
        WSABUF buffers[2] = {{ULONG(header.size()), reinterpret_cast<char*>(header.data())},
                             {ULONG(payload), reinterpret_cast<char*>(
                                                  const_cast<uint8_t*>(frame.pixels.data() + offset))}};
        DWORD sent = 0;
        if (WSASend(socket, buffers, 2, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (transient_socket_error(error))
                return result; // Abandon this frame; never wait behind stale pixels.
            throw std::runtime_error("UDP frame send failed with Winsock error " +
                                     std::to_string(error));
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
    constexpr uint64_t session = 0x0102030405060708ull;
    CapturedFrame frame;
    frame.width = 320;
    frame.height = 160;
    frame.capture_ns = 0x0011223344556677ll;
    frame.pixels.resize(size_t(frame.width) * frame.height * 4);
    const uint16_t count = uint16_t((frame.pixels.size() + frame_stride - 1) / frame_stride);
    auto header = encode_frame_header(frame, session, 0x89abcdefu, uint16_t(count - 1), count,
                                      uint32_t(count - 1) * frame_stride);
    const bool valid = !std::memcmp(header.data(), "UVF1", 4) && header[4] == 1 &&
                       header[5] == 2 && get16(header.data() + 6) == frame_header_size &&
                       get64(header.data() + 8) == session &&
                       get32(header.data() + 16) == 0x89abcdefu &&
                       get64(header.data() + 20) == uint64_t(frame.capture_ns) &&
                       get16(header.data() + 28) == frame.width &&
                       get16(header.data() + 30) == frame.height &&
                       get32(header.data() + 32) == frame.pixels.size() &&
                       get16(header.data() + 36) == count - 1 &&
                       get16(header.data() + 38) == count &&
                       get32(header.data() + 40) == uint32_t(count - 1) * frame_stride &&
                       get16(header.data() + 44) == frame_stride &&
                       get16(header.data() + 46) == 0;
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
                const int length = recv(socket_, reinterpret_cast<char*>(packet.data()),
                                        int(packet.size()), 0);
                const int64_t received = now_ns();
                if (length == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (transient_socket_error(error))
                        continue;
                    throw std::runtime_error("UDP control receive failed");
                }
                if (length == 24 && !std::memcmp(packet.data(), "UVC1", 4) &&
                    get32(packet.data() + 4) == 0 && get64(packet.data() + 8) == session_) {
                    std::array<uint8_t, 40> reply{};
                    std::memcpy(reply.data(), "UVS1", 4);
                    put64(reply.data() + 8, session_);
                    put64(reply.data() + 16, get64(packet.data() + 16));
                    put64(reply.data() + 24, uint64_t(received));
                    put64(reply.data() + 32, uint64_t(now_ns()));
                    send_control(socket_, reply.data(), reply.size());
                } else if (accept_resolution_ && length == 24 &&
                           !std::memcmp(packet.data(), "UVN1", 4) && packet[4] == 1 &&
                           packet[5] == 2 && get16(packet.data() + 6) == 0 &&
                           get64(packet.data() + 8) == session_ &&
                           get32(packet.data() + 20) == 0) {
                    const uint16_t width = get16(packet.data() + 16);
                    const uint16_t height = get16(packet.data() + 18);
                    if (width && height && width <= 1024 && height <= 1024)
                        desired_.store(pack(width, height), std::memory_order_release);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Control channel stopped: " << error.what() << '\n';
            stop_.store(true, std::memory_order_relaxed);
        }
    }

  public:
    ControlPlane(SOCKET socket, uint64_t session, uint16_t width, uint16_t height,
                 bool accept_resolution)
        : socket_(socket), session_(session), accept_resolution_(accept_resolution),
          desired_(pack(width, height)), thread_([this] { run(); }) {}

    ~ControlPlane() {
        stop();
    }

    void stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
    }

    bool stopped() const {
        return stop_.load(std::memory_order_relaxed);
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

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.list_outputs) {
            list_outputs();
            return 0;
        }
        if (options.self_test) {
            self_test();
            return 0;
        }
        Network network(options.host, options.port, options.send_buffer_mib);
        const uint64_t session = random_session();
        auto capture = std::make_unique<DesktopCapture>(options.output);
        uint16_t width = options.width, height = options.height;
        if (!capture->fits(width, height))
            throw std::runtime_error("Initial crop is larger than the selected output");
        std::atomic<bool> stop{false};
        ConsoleHandlerGuard console(stop);
        ControlPlane control(network.socket, session, options.width, options.height,
                             options.sync_resolution);
        std::cout << "Sending exact center crop " << width << 'x' << height << " BGRA to "
                  << options.host << ':' << options.port
                  << (options.sync_resolution ? " (resolution sync enabled)" : "") << "\n"
                  << "Press Ctrl+C to stop.\n";

        CapturedFrame frame;
        uint32_t sequence = 0;
        uint64_t captured = 0, sent_frames = 0, dropped = 0, skipped = 0, datagrams = 0,
                 wire_bytes = 0;
        int64_t report_at = now_ns() + 1'000'000'000;
        int64_t next_send = 0, backpressure_until = 0;
        const int64_t interval = options.fps > 0 ? int64_t(1e9 / options.fps) : 0;

        while (!stop.load(std::memory_order_relaxed) && !control.stopped()) {
            auto [requested_width, requested_height] = control.desired();
            if ((requested_width != width || requested_height != height) &&
                capture->fits(requested_width, requested_height)) {
                width = requested_width;
                height = requested_height;
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
                capture = std::make_unique<DesktopCapture>(options.output);
                continue;
            }
            ++captured;
            const uint32_t frame_sequence = sequence++;
            const int64_t now = now_ns();
            if ((interval && now < next_send) || now < backpressure_until) {
                ++skipped;
                continue;
            }
            next_send = interval ? now + interval : 0;
            const SendResult result = send_frame(network.socket, frame, session, frame_sequence);
            datagrams += result.datagrams;
            wire_bytes += result.bytes;
            if (result.complete)
                ++sent_frames;
            else {
                ++dropped;
                // Let already-queued fragments drain before attempting another complete frame.
                // One bit/ns is a conservative 1 Gbit/s recovery estimate; faster links pay this
                // cost only after actual backpressure.
                const int64_t recovery =
                    std::max<int64_t>(1'000'000, int64_t(frame.pixels.size() * 8));
                backpressure_until = now + recovery;
            }

            if (now >= report_at) {
                static uint64_t previous_captured = 0, previous_sent = 0, previous_bytes = 0;
                std::cout << "capture " << (captured - previous_captured) << " fps, sent "
                          << (sent_frames - previous_sent) << " fps, " << skipped << " paced/skipped, "
                          << dropped << " backpressure drops, " << std::fixed << std::setprecision(1)
                          << double(wire_bytes - previous_bytes) * 8 / 1e6 << " Mbit/s, "
                          << width << 'x' << height << '\n';
                previous_captured = captured;
                previous_sent = sent_frames;
                previous_bytes = wire_bytes;
                report_at = now + 1'000'000'000;
            }
        }
        stop.store(true, std::memory_order_relaxed);
        control.stop();
        std::cout << "Stopped. Captured " << captured << ", sent " << sent_frames << ", dropped "
                  << dropped << ", paced/skipped " << skipped << ", datagrams " << datagrams << ".\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frame_sender: " << error.what() << '\n';
        return 1;
    }
}
