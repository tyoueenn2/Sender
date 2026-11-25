#pragma once

#include <atomic>

namespace frame_sender {

enum class VideoSendStage { starting, waiting_for_crop, waiting_for_first_frame, frames_sent_locally };

// UI snapshot written by the capture worker, except for reset before starting.
// "Sent" means all UDP fragments were accepted by the local socket for at least
// one frame in this session. It says nothing about delivery or decoding.
class VideoSendProgress {
    std::atomic<VideoSendStage> stage_{VideoSendStage::starting};

  public:
    void reset() { stage_.store(VideoSendStage::starting, std::memory_order_relaxed); }
    void capture_ready(bool automatic_crop) {
        stage_.store(automatic_crop ? VideoSendStage::waiting_for_crop :
                     VideoSendStage::waiting_for_first_frame, std::memory_order_relaxed);
    }
    void crop_ready() {
        stage_.store(VideoSendStage::waiting_for_first_frame, std::memory_order_relaxed);
    }
    void first_frame_sent() {
        stage_.store(VideoSendStage::frames_sent_locally, std::memory_order_relaxed);
    }
    VideoSendStage stage() const { return stage_.load(std::memory_order_relaxed); }
};

} // namespace frame_sender
