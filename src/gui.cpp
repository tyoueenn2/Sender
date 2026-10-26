#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <ws2tcpip.h>
#include "sender.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {
enum ControlId {
    title = 100, subtitle, host_label, host, port_label, port, display_label, display,
    refresh, method_label, method, description, automatic, width_label, width,
    height_label, height, fps_label, fps, start, stop, status, log_label, activity
};

std::wstring wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Both capture and control threads write here. The UI drains it on its timer;
// neither thread sends synchronous window messages or touches a UI control.
class LogBuffer : public std::streambuf {
    std::mutex mutex_;
    std::string pending_;
    static constexpr size_t limit = 64 * 1024;

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0) return 0;
        std::lock_guard lock(mutex_);
        const size_t size = size_t(count);
        pending_.append(data + (size > limit ? size - limit : 0), std::min(size, limit));
        if (pending_.size() > limit)
            pending_.erase(0, pending_.size() - limit);
        return count;
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            const char c = traits_type::to_char_type(value);
            xsputn(&c, 1);
        }
        return traits_type::not_eof(value);
    }
    int sync() override { return 0; }

  public:
    std::string take() {
        std::lock_guard lock(mutex_);
        return std::exchange(pending_, {});
    }
};

class Window {
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, title_font_ = nullptr;
    UINT dpi_ = 96;
    std::vector<frame_sender::DisplayInfo> displays_;
    std::thread worker_;
    std::atomic<bool> stop_{false}, finished_{false};
    int result_ = 0;
    bool closing_ = false;
    LogBuffer buffer_;
    std::streambuf* original_out_ = nullptr;
    std::streambuf* original_error_ = nullptr;

    int scale(int value) const { return MulDiv(value, int(dpi_), 96); }
    HWND control(int id) const { return GetDlgItem(hwnd_, id); }
    void text(int id, const wchar_t* value) { SetWindowTextW(control(id), value); }
    bool checked() const { return SendMessageW(control(automatic), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    std::wstring value(int id) const {
        const HWND field = control(id);
        std::wstring result(size_t(GetWindowTextLengthW(field)) + 1, L'\0');
        const int length = GetWindowTextW(field, result.data(), int(result.size()));
        result.resize(size_t(length));
        return result;
    }
    void add(int id, const wchar_t* class_name, const wchar_t* caption, DWORD style = 0, DWORD ex = 0) {
        if (!CreateWindowExW(ex, class_name, caption, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr))
            throw std::runtime_error("Could not create a window control");
    }
    void append_log(std::string_view message) {
        std::wstring lines;
        for (const wchar_t c : wide(message)) {
            if (c == L'\n') lines += L'\r';
            lines += c;
        }
        const HWND log = control(activity);
        if (GetWindowTextLengthW(log) + lines.size() > 48 * 1024)
            SetWindowTextW(log, L"");
        SendMessageW(log, EM_SETSEL, WPARAM(-1), LPARAM(-1));
        SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(lines.c_str()));
        SendMessageW(log, EM_SCROLLCARET, 0, 0);
    }
    void fonts() {
        const HFONT old = font_, old_title = title_font_;
        font_ = CreateFontW(-MulDiv(10, int(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        title_font_ = CreateFontW(-MulDiv(19, int(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        for (int id = title; id <= activity; ++id)
            SendMessageW(control(id), WM_SETFONT, reinterpret_cast<WPARAM>(id == title ? title_font_ : font_), TRUE);
        if (old) DeleteObject(old);
        if (old_title) DeleteObject(old_title);
    }
    void layout() {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        const int w = MulDiv(rect.right, 96, int(dpi_));
        const int h = MulDiv(rect.bottom, 96, int(dpi_));
        auto place = [&](int id, int x, int y, int width, int height) {
            MoveWindow(control(id), scale(x), scale(y), scale(width), scale(height), TRUE);
        };
        place(title, 24, 16, w - 48, 36);
        place(subtitle, 24, 58, w - 48, 24);
        place(host_label, 24, 98, w - 200, 22);
        place(host, 24, 122, w - 192, 28);
        place(port_label, w - 144, 98, 120, 22);
        place(port, w - 144, 122, 120, 28);
        place(display_label, 24, 166, w - 48, 22);
        place(display, 24, 190, w - 160, 220);
        place(refresh, w - 120, 189, 96, 30);
        place(method_label, 24, 234, w - 48, 22);
        place(method, 24, 258, w - 48, 180);
        place(description, 24, 294, w - 48, 56);
        place(automatic, 24, 356, w - 48, 26);
        place(width_label, 24, 394, 110, 22);
        place(width, 24, 418, 110, 28);
        place(height_label, 150, 394, 110, 22);
        place(height, 150, 418, 110, 28);
        place(fps_label, w - 184, 394, 160, 22);
        place(fps, w - 184, 418, 160, 28);
        place(start, 24, 466, 140, 36);
        place(stop, 176, 466, 88, 36);
        place(status, 280, 473, w - 304, 28);
        place(log_label, 24, 520, w - 48, 22);
        place(activity, 24, 546, w - 48, std::max(64, h - 570));
    }
    void method_description() {
        const auto index = SendMessageW(control(method), CB_GETCURSEL, 0, 0);
        if (index >= 0 && size_t(index) < frame_sender::capture_methods.size())
            text(description, wide(frame_sender::capture_methods[size_t(index)].description).c_str());
    }
    void enable_settings(bool enable) {
        for (int id : {host, port, display, refresh, method, automatic, fps})
            EnableWindow(control(id), enable);
        EnableWindow(control(width), enable && !checked());
        EnableWindow(control(height), enable && !checked());
        EnableWindow(control(start), enable && !displays_.empty());
        EnableWindow(control(stop), !enable && !closing_);
    }
    void refresh_displays() {
        unsigned previous = 0;
        const auto selected = SendMessageW(control(display), CB_GETCURSEL, 0, 0);
        if (selected >= 0 && size_t(selected) < displays_.size())
            previous = displays_[size_t(selected)].index;
        try {
            displays_ = frame_sender::display_choices();
            SendMessageW(control(display), CB_RESETCONTENT, 0, 0);
            int select = 0;
            for (size_t i = 0; i < displays_.size(); ++i) {
                SendMessageW(control(display), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displays_[i].label.c_str()));
                if (displays_[i].index == previous) select = int(i);
            }
            SendMessageW(control(display), CB_SETCURSEL, WPARAM(select), 0);
            text(status, displays_.empty() ? L"No attached display" : L"Ready");
        } catch (const std::exception& error) {
            displays_.clear();
            SendMessageW(control(display), CB_RESETCONTENT, 0, 0);
            text(status, L"Display lookup failed");
            append_log(std::string(error.what()) + "\n");
        }
        enable_settings(true);
    }
    frame_sender::Options options() const {
        const auto address = narrow(value(host));
        IN_ADDR ipv4{};
        if (InetPtonA(AF_INET, address.c_str(), &ipv4) != 1)
            throw std::runtime_error("Enter a Receiver IPv4 address, for example 192.168.1.10.");
        const auto selected_display = SendMessageW(control(display), CB_GETCURSEL, 0, 0);
        const auto selected_method = SendMessageW(control(method), CB_GETCURSEL, 0, 0);
        if (selected_display < 0 || size_t(selected_display) >= displays_.size())
            throw std::runtime_error("Select an attached display, or click Refresh.");
        if (selected_method < 0 || size_t(selected_method) >= frame_sender::capture_methods.size())
            throw std::runtime_error("Select a capture method.");
        // Use the CLI parser, so the GUI has exactly the same option bounds.
        std::vector<std::string> args{"frame_sender", "--host", address,
            "--port", narrow(value(port)), "--output", std::to_string(displays_[size_t(selected_display)].index),
            "--capture", std::string(frame_sender::capture_methods[size_t(selected_method)].name),
            "--fps", narrow(value(fps)), checked() ? "--sync-resolution" : "--no-sync-resolution"};
        if (!checked()) {
            args.insert(args.end(), {"--width", narrow(value(width)), "--height", narrow(value(height))});
        }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        return frame_sender::parse_arguments(int(argv.size()), argv.data());
    }
    void start_sender() {
        if (worker_.joinable()) return;
        try {
            const auto settings = options();
            stop_.store(false);
            finished_.store(false);
            SetWindowTextW(control(activity), L"");
            buffer_.take();
            original_out_ = std::cout.rdbuf(&buffer_);
            original_error_ = std::cerr.rdbuf(&buffer_);
            try {
                worker_ = std::thread([this, settings] {
                    result_ = frame_sender::run_sender(settings, stop_);
                    finished_.store(true, std::memory_order_release);
                });
            } catch (...) {
                restore_streams();
                throw;
            }
            enable_settings(false);
            text(status, L"Starting sender...");
        } catch (const std::exception& error) {
            text(status, L"Check settings");
            append_log(std::string(error.what()) + "\n");
        }
    }
    void request_stop() {
        if (!worker_.joinable()) return;
        stop_.store(true, std::memory_order_relaxed);
        EnableWindow(control(stop), FALSE);
        text(status, L"Stopping...");
    }
    void restore_streams() {
        if (original_out_) std::cout.rdbuf(std::exchange(original_out_, nullptr));
        if (original_error_) std::cerr.rdbuf(std::exchange(original_error_, nullptr));
    }
    void tick() {
        const auto messages = buffer_.take();
        if (!messages.empty()) {
            append_log(messages);
            if (!stop_.load()) {
                if (messages.find("Waiting for Receiver") != std::string::npos)
                    text(status, L"Waiting for Receiver's crop");
                if (messages.find("Sending fixed") != std::string::npos ||
                    messages.find("Receiver requested") != std::string::npos ||
                    messages.find(" fps, sent ") != std::string::npos)
                    text(status, L"Sending");
            }
        }
        if (worker_.joinable() && finished_.load(std::memory_order_acquire)) {
            worker_.join();
            restore_streams();
            append_log(buffer_.take());
            if (closing_) {
                DestroyWindow(hwnd_);
                return;
            }
            enable_settings(true);
            text(status, result_ == 0 ? L"Stopped" : L"Sender failed - see activity");
        }
    }
    void create() {
        dpi_ = GetDpiForWindow(hwnd_);
        add(title, L"STATIC", L"FrameSender");
        add(subtitle, L"STATIC", L"Send an exact desktop crop to your Receiver.");
        add(host_label, L"STATIC", L"Receiver &IPv4");
        add(host, L"EDIT", L"127.0.0.1", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        add(port_label, L"STATIC", L"UDP &port");
        add(port, L"EDIT", L"5000", WS_TABSTOP | ES_NUMBER, WS_EX_CLIENTEDGE);
        add(display_label, L"STATIC", L"&Display");
        add(display, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
        add(refresh, L"BUTTON", L"&Refresh", WS_TABSTOP | BS_PUSHBUTTON);
        add(method_label, L"STATIC", L"&Capture method");
        add(method, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST);
        add(description, L"STATIC", L"");
        add(automatic, L"BUTTON", L"Follow Receiver's crop &automatically", WS_TABSTOP | BS_AUTOCHECKBOX);
        add(width_label, L"STATIC", L"Crop &width");
        add(width, L"EDIT", L"320", WS_TABSTOP | ES_NUMBER, WS_EX_CLIENTEDGE);
        add(height_label, L"STATIC", L"Crop &height");
        add(height, L"EDIT", L"320", WS_TABSTOP | ES_NUMBER, WS_EX_CLIENTEDGE);
        add(fps_label, L"STATIC", L"Max &FPS (0 = auto)");
        add(fps, L"EDIT", L"0", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        add(start, L"BUTTON", L"&Start sending", WS_TABSTOP | BS_DEFPUSHBUTTON);
        add(stop, L"BUTTON", L"S&top", WS_TABSTOP | BS_PUSHBUTTON);
        add(status, L"STATIC", L"Ready");
        add(log_label, L"STATIC", L"Activity");
        add(activity, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            WS_EX_CLIENTEDGE);
        SendMessageW(control(host), EM_SETLIMITTEXT, 15, 0);
        for (int id : {port, width, height, fps})
            SendMessageW(control(id), EM_SETLIMITTEXT, 16, 0);
        SendMessageW(control(activity), EM_SETLIMITTEXT, 64 * 1024, 0);
        SendMessageW(control(automatic), BM_SETCHECK, BST_CHECKED, 0);
        for (const auto* name : {L"DXGI Desktop Duplication", L"Windows Graphics Capture", L"GDI BitBlt"})
            SendMessageW(control(method), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(control(method), CB_SETCURSEL, 0, 0);
        fonts();
        layout();
        method_description();
        refresh_displays();
        append_log("Choose your Receiver's IPv4 address, then click Start sending.\n"
                   "Automatic crop waits for Receiver to load its model.\n");
        if (!SetTimer(hwnd_, 1, 100, nullptr))
            throw std::runtime_error("Could not create the UI update timer");
    }

  public:
    ~Window() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        restore_streams();
        if (font_) DeleteObject(font_);
        if (title_font_) DeleteObject(title_font_);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        Window* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wp, lp);
        try {
            switch (message) {
            case WM_CREATE: self->create(); return 0;
            case WM_SIZE: if (wp != SIZE_MINIMIZED) self->layout(); return 0;
            case WM_DPICHANGED: {
                self->dpi_ = HIWORD(wp);
                const auto* rect = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                             rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->fonts(); self->layout(); return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
                limits->ptMinTrackSize = {self->scale(656), self->scale(680)};
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == start && HIWORD(wp) == BN_CLICKED) self->start_sender();
                if (LOWORD(wp) == stop && HIWORD(wp) == BN_CLICKED) self->request_stop();
                if (LOWORD(wp) == refresh && HIWORD(wp) == BN_CLICKED && !self->worker_.joinable()) self->refresh_displays();
                if (LOWORD(wp) == method && HIWORD(wp) == CBN_SELCHANGE) self->method_description();
                if (LOWORD(wp) == automatic && HIWORD(wp) == BN_CLICKED)
                    self->enable_settings(!self->worker_.joinable());
                return 0;
            case WM_TIMER: self->tick(); return 0;
            case WM_CLOSE:
                if (self->worker_.joinable()) {
                    self->closing_ = true;
                    self->request_stop();
                    self->enable_settings(false);
                } else DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
            }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, wide(error.what()).c_str(), L"FrameSender", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) return -1;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Window::procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"FrameSenderWindow";
    if (!RegisterClassW(&wc)) return 1;
    Window state;
    const UINT dpi = GetDpiForSystem();
    const HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"FrameSender",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(656, int(dpi), 96),
        MulDiv(760, int(dpi), 96), nullptr, nullptr, instance, &state);
    if (!hwnd) return 1;
    // CI verifies native controls and defaults without starting capture/networking.
    if (std::wstring_view(command_line) == L"--self-test") {
        const bool valid = GetDlgItem(hwnd, start) && GetDlgItem(hwnd, activity) &&
            SendDlgItemMessageW(hwnd, method, CB_GETCOUNT, 0, 0) == 3 &&
            SendDlgItemMessageW(hwnd, automatic, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            !IsWindowEnabled(GetDlgItem(hwnd, width)) && !IsWindowEnabled(GetDlgItem(hwnd, stop));
        DestroyWindow(hwnd);
        return valid ? 0 : 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG message{};
    int result;
    while ((result = int(GetMessageW(&message, nullptr, 0, 0))) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result < 0 ? 1 : int(message.wParam);
}
