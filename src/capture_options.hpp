#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace frame_sender {
enum class CaptureMethod { dxgi, wgc, gdi };

struct CaptureMethodInfo {
    CaptureMethod method;
    std::string_view name;
    std::string_view description;
};

inline constexpr std::array capture_methods{
    CaptureMethodInfo{CaptureMethod::dxgi, "dxgi",
                      "DXGI Desktop Duplication (default): GPU desktop capture with presentation "
                      "timestamps; unrotated displays only."},
    CaptureMethodInfo{CaptureMethod::wgc, "wgc",
                      "Windows Graphics Capture: GPU monitor capture on Windows 10 2004+; "
                      "keeps the system capture border and excludes the cursor."},
    CaptureMethodInfo{CaptureMethod::gdi, "gdi",
                      "GDI BitBlt: CPU-readable desktop snapshots for compatibility; polls at "
                      "60 FPS by default and may miss hardware overlays or exclusive fullscreen."}};

inline std::optional<CaptureMethod> parse_capture_method(std::string_view name) {
    for (const auto& info : capture_methods)
        if (info.name == name)
            return info.method;
    return std::nullopt;
}

struct Crop {
    uint32_t left, top, width, height;
};

inline std::optional<Crop> center_crop(uint32_t source_width, uint32_t source_height,
                                       uint32_t width, uint32_t height) {
    if (!width || !height || width > source_width || height > source_height)
        return std::nullopt;
    return Crop{(source_width - width) / 2, (source_height - height) / 2, width, height};
}
} // namespace frame_sender
