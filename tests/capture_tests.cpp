#include "../src/capture_options.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        using namespace frame_sender;
        check(parse_capture_method("dxgi") == CaptureMethod::dxgi, "DXGI selection failed");
        check(parse_capture_method("wgc") == CaptureMethod::wgc, "WGC selection failed");
        check(parse_capture_method("gdi") == CaptureMethod::gdi, "GDI selection failed");
        for (const auto name : {"", "auto", "DXGI", "printwindow", "gdi ", "unknown"})
            check(!parse_capture_method(name), "Invalid capture method was accepted");
        for (size_t i = 0; i < capture_methods.size(); ++i) {
            const auto& info = capture_methods[i];
            check(!info.description.empty(), "Capture method lacks a description");
            for (size_t j = 0; j < i; ++j)
                check(info.name != capture_methods[j].name, "Duplicate method name");
        }

        auto crop = center_crop(1920, 1080, 320, 320);
        check(crop && crop->left == 800 && crop->top == 380 &&
              crop->width == 320 && crop->height == 320, "Square crop is not centered");
        crop = center_crop(1920, 1080, 321, 159);
        check(crop && crop->left == 799 && crop->top == 460, "Odd rectangular crop changed pixels");
        check(-1920 + int(crop->left) == -1121 && -1080 + int(crop->top) == -620,
              "Negative monitor origin was lost");
        crop = center_crop(1080, 1920, 159, 321);
        check(crop && crop->left == 460 && crop->top == 799, "Portrait crop is not centered");
        crop = center_crop(1, 1, 1, 1);
        check(crop && crop->left == 0 && crop->top == 0, "1x1 crop failed");
        crop = center_crop(1920, 1080, 1920, 1080);
        check(crop && crop->left == 0 && crop->top == 0, "Full display crop failed");
        for (const auto dimensions : {std::array<uint32_t, 4>{1920, 1080, 0, 320},
                                     {1920, 1080, 320, 0}, {1920, 1080, 1921, 320},
                                     {1920, 1080, 320, 1081}, {0, 0, 1, 1}})
            check(!center_crop(dimensions[0], dimensions[1], dimensions[2], dimensions[3]),
                  "Invalid crop was accepted");
        crop = center_crop(std::numeric_limits<uint32_t>::max(), 1080, 1, 1);
        check(crop && crop->left == 2147483647u, "Large source dimensions overflowed");
        std::cout << "Capture selection, descriptions and exact center-crop geometry passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
