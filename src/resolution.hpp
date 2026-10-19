#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace frame_sender {
struct Resolution {
    uint16_t width = 0, height = 0;
    bool operator==(const Resolution&) const = default;
};

// Accept only exact UVN1 messages for this capture session. The connected UDP
// socket additionally restricts datagrams to the configured Receiver endpoint.
inline std::optional<Resolution> parse_resolution_request(std::span<const uint8_t> packet,
                                                         uint64_t session) {
    if (packet.size() != 24 || !session || std::memcmp(packet.data(), "UVN1", 4) ||
        packet[4] != 1 || packet[5] != 2 || packet[6] || packet[7] ||
        packet[20] || packet[21] || packet[22] || packet[23])
        return std::nullopt;
    uint64_t received_session = 0;
    for (std::size_t i = 8; i < 16; ++i)
        received_session = (received_session << 8) | packet[i];
    const Resolution result{uint16_t(uint16_t(packet[16]) << 8 | packet[17]),
                            uint16_t(uint16_t(packet[18]) << 8 | packet[19])};
    if (received_session != session || !result.width || !result.height ||
        result.width > 1024 || result.height > 1024)
        return std::nullopt;
    return result;
}
} // namespace frame_sender
