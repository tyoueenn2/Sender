#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Mirrored in Receiver/include/receiver/cursor_protocol.hpp.
namespace cursor_protocol {
inline uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t u64(const uint8_t* p) {
    return uint64_t(u32(p)) << 32 | u32(p + 4);
}
inline int32_t i32(const uint8_t* p) {
    uint32_t n = u32(p);
    return n <= 0x7fffffffu ? int32_t(n) : int32_t(int64_t(n) - 0x100000000ll);
}
inline void put32(uint8_t* p, uint32_t n) {
    for (int i = 3; i >= 0; --i) {
        p[i] = uint8_t(n);
        n >>= 8;
    }
}
inline void put64(uint8_t* p, uint64_t n) {
    put32(p, uint32_t(n >> 32));
    put32(p + 4, uint32_t(n));
}
inline std::array<uint8_t, 24> request(uint64_t session, uint64_t token) {
    std::array<uint8_t, 24> p{};
    std::memcpy(p.data(), "UVP1", 4);
    p[4] = 1;
    put64(p.data() + 8, session);
    put64(p.data() + 16, token);
    return p;
}
inline bool parse_request(const uint8_t* p, size_t n, uint64_t session, uint64_t& token) {
    if (n != 24 || std::memcmp(p, "UVP1", 4) || p[4] != 1 || p[5] || p[6] || p[7] || !session ||
        u64(p + 8) != session)
        return false;
    token = u64(p + 16);
    return token != 0;
}
struct Sample {
    uint8_t flags = 0;
    uint64_t session = 0, token = 0, sample_ns = 0, frame_ns = 0, display = 0;
    uint32_t sequence = 0, generation = 0, frame_sequence = 0;
    int32_t x = 0, y = 0, left = 0, top = 0, width = 0, height = 0, crop_left = 0, crop_top = 0,
            crop_width = 0, crop_height = 0;
    bool valid() const {
        return (flags & 1) != 0;
    }
    bool has_frame() const {
        return (flags & 4) != 0;
    }
};
inline std::array<uint8_t, 112> packet(const Sample& s) {
    std::array<uint8_t, 112> p{};
    std::memcpy(p.data(), "UVG1", 4);
    p[4] = 1;
    p[5] = s.flags;
    put64(p.data() + 8, s.session);
    put64(p.data() + 16, s.token);
    put64(p.data() + 24, s.sample_ns);
    put32(p.data() + 32, s.sequence);
    put32(p.data() + 36, s.generation);
    int32_t values[] = {s.x,      s.y,         s.left,     s.top,        s.width,
                        s.height, s.crop_left, s.crop_top, s.crop_width, s.crop_height};
    for (size_t i = 0; i < 10; ++i)
        put32(p.data() + 40 + i * 4, uint32_t(values[i]));
    put32(p.data() + 80, s.frame_sequence);
    put64(p.data() + 88, s.frame_ns);
    put64(p.data() + 96, s.display);
    return p;
}
inline bool parse(const uint8_t* p, size_t n, Sample& s) {
    if (n != 112 || std::memcmp(p, "UVG1", 4) || p[4] != 1 || (p[5] & ~7u) || p[6] || p[7] || u32(p + 84) ||
        u64(p + 104))
        return false;
    s.flags = p[5];
    s.session = u64(p + 8);
    s.token = u64(p + 16);
    s.sample_ns = u64(p + 24);
    s.sequence = u32(p + 32);
    s.generation = u32(p + 36);
    s.x = i32(p + 40);
    s.y = i32(p + 44);
    s.left = i32(p + 48);
    s.top = i32(p + 52);
    s.width = i32(p + 56);
    s.height = i32(p + 60);
    s.crop_left = i32(p + 64);
    s.crop_top = i32(p + 68);
    s.crop_width = i32(p + 72);
    s.crop_height = i32(p + 76);
    s.frame_sequence = u32(p + 80);
    s.frame_ns = u64(p + 88);
    s.display = u64(p + 96);
    return s.session && s.token && s.generation && s.width > 0 && s.height > 0 && s.width <= 65536 &&
           s.height <= 65536 &&
           (!s.has_frame() ||
            (s.crop_width > 0 && s.crop_height > 0 && s.crop_width <= 1024 && s.crop_height <= 1024 &&
             int64_t(s.crop_left) >= s.left && int64_t(s.crop_top) >= s.top &&
             int64_t(s.crop_left) + s.crop_width <= int64_t(s.left) + s.width &&
             int64_t(s.crop_top) + s.crop_height <= int64_t(s.top) + s.height));
}
} // namespace cursor_protocol
