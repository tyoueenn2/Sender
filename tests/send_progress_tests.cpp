#include "../src/send_progress.hpp"
#include <iostream>

int main() {
    using frame_sender::VideoSendProgress;
    using frame_sender::VideoSendStage;
    VideoSendProgress progress;
    const auto expect = [&](VideoSendStage stage, const char* description) {
        if (progress.stage() == stage)
            return true;
        std::cerr << description << '\n';
        return false;
    };
    if (!expect(VideoSendStage::starting, "A new session starts before capture is ready")) return 1;
    progress.capture_ready(true);
    if (!expect(VideoSendStage::waiting_for_crop, "Automatic crop must wait for Receiver's crop size")) return 1;
    progress.crop_ready();
    if (!expect(VideoSendStage::waiting_for_first_frame, "An available crop does not imply a frame was sent")) return 1;
    progress.first_frame_sent();
    if (!expect(VideoSendStage::frames_sent_locally, "A complete local send is a separate stage")) return 1;
    progress.reset();
    if (!expect(VideoSendStage::starting, "A restart must clear the previous session's send status")) return 1;
    progress.capture_ready(false);
    if (!expect(VideoSendStage::waiting_for_first_frame, "Manual crop does not wait for Receiver to send a size")) return 1;
    return 0;
}
