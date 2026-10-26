#include "../src/sender.hpp"

#include <initializer_list>
#include <iostream>
#include <stdexcept>

frame_sender::Options parse(std::initializer_list<const char*> values) {
    std::vector<std::string> args{"frame_sender"};
    for (const char* value : values) args.emplace_back(value);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return frame_sender::parse_arguments(int(argv.size()), argv.data());
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const auto defaults = parse({});
        check(defaults.sync_resolution && defaults.width == 320 && defaults.height == 320 &&
              defaults.capture_method == frame_sender::CaptureMethod::dxgi && defaults.fps == 0,
              "GUI/CLI defaults diverged");
        const auto manual = parse({"--host", "192.168.1.10", "--port", "65535", "--output", "2",
            "--capture", "gdi", "--no-sync-resolution", "--width", "321", "--height", "159", "--fps", "29.97"});
        check(manual.host == "192.168.1.10" && manual.port == 65535 && manual.output == 2 &&
              !manual.sync_resolution && manual.width == 321 && manual.height == 159 &&
              manual.capture_method == frame_sender::CaptureMethod::gdi && manual.fps == 29.97,
              "Manual GUI settings changed");
        const auto automatic = parse({"--capture", "wgc", "--sync-resolution", "--fps", "120"});
        check(automatic.sync_resolution && automatic.capture_method == frame_sender::CaptureMethod::wgc &&
              automatic.fps == 120, "Automatic GUI settings changed");
        for (const char* option : {"--port", "--width", "--height"}) {
            for (const char* value : {"0", "65536", "invalid", ""}) {
                bool rejected = false;
                try { parse({option, value}); } catch (const std::exception&) { rejected = true; }
                check(rejected, "Invalid numeric setting was accepted");
            }
        }
        for (const char* value : {"nan", "inf", "-1", "1001", ""}) {
            bool rejected = false;
            try { parse({"--fps", value}); } catch (const std::exception&) { rejected = true; }
            check(rejected, "Invalid FPS setting was accepted");
        }
        std::cout << "GUI/CLI defaults, manual and automatic settings, and invalid fields passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
