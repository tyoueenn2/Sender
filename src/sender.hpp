#pragma once

#include "capture_options.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace frame_sender {
struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 5000;
    uint16_t width = 320;
    uint16_t height = 320;
    unsigned output = 0;
    CaptureMethod capture_method = CaptureMethod::dxgi;
    double fps = 0;
    int send_buffer_mib = 2;
    bool sync_resolution = true;
    bool list_outputs = false;
    bool list_capture_methods = false;
    bool self_test = false;
};

struct DisplayInfo {
    unsigned index;
    std::wstring label;
};

std::vector<DisplayInfo> display_choices();
Options parse_arguments(int argc, char** argv);
int run_sender(const Options& options, std::atomic<bool>& stop);
int run_cli(int argc, char** argv);
} // namespace frame_sender
