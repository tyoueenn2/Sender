#include "../src/resolution.hpp"
#include "../src/cursor.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using frame_sender::parse_resolution_request;
using frame_sender::Resolution;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        constexpr uint64_t session = 0x0102030405060708ull;
        // Independent golden vector shared with Receiver's protocol test.
        constexpr std::array<uint8_t, 24> wire{'U', 'V', 'N', '1', 1,    2,    0,    0,    1, 2, 3, 4,
                                               5,   6,   7,   8,   0x02, 0x80, 0x01, 0x40, 0, 0, 0, 0};
        check(parse_resolution_request(wire, session) == Resolution{640, 320},
              "Rectangular BGRA request failed");
        check(!parse_resolution_request(wire, session + 1), "Wrong session was accepted");
        check(!parse_resolution_request(wire, 0), "Zero session was accepted");
        for (size_t length = 0; length < wire.size(); ++length)
            check(!parse_resolution_request(std::span(wire).first(length), session),
                  "Truncated packet was accepted");
        auto oversized = std::vector<uint8_t>(wire.begin(), wire.end());
        oversized.push_back(0);
        check(!parse_resolution_request(oversized, session), "Oversized packet was accepted");
        for (size_t offset : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 20u, 21u, 22u, 23u}) {
            auto bad = wire;
            bad[offset] ^= 1;
            check(!parse_resolution_request(bad, session), "Invalid header was accepted");
        }
        for (size_t offset : {16u, 18u}) {
            for (unsigned dimension : {0u, 1025u, 65535u}) {
                auto bad = wire;
                bad[offset] = uint8_t(dimension >> 8);
                bad[offset + 1] = uint8_t(dimension);
                check(!parse_resolution_request(bad, session), "Invalid crop dimension was accepted");
            }
        }
        auto bounds = wire;
        bounds[16] = 0;
        bounds[17] = 1;
        bounds[18] = 4;
        bounds[19] = 0;
        check(parse_resolution_request(bounds, session) == Resolution{1, 1024},
              "Valid dimension bounds failed");
        auto query = cursor_protocol::request(session, 42);
        uint64_t token = 0;
        check(cursor_protocol::parse_request(query.data(), query.size(), session, token) && token == 42,
              "Cursor token mismatch");
        check(!cursor_protocol::parse_request(query.data(), query.size(), session + 1, token),
              "Wrong cursor session accepted");
        cursor_protocol::Sample sample;
        sample.session = session;
        sample.token = 42;
        sample.generation = 1;
        sample.flags = 5;
        sample.left = -1920;
        sample.top = 0;
        sample.width = 1920;
        sample.height = 1080;
        sample.x = -960;
        sample.y = 540;
        sample.crop_left = -1024;
        sample.crop_top = 476;
        sample.crop_width = sample.crop_height = 128;
        auto response = cursor_protocol::packet(sample);
        cursor_protocol::Sample parsed;
        check(response.size() == 112 && response[48] == 255 && response[51] == 128,
              "Cursor geometry wire mismatch");
        check(cursor_protocol::parse(response.data(), response.size(), parsed) && parsed.x == -960,
              "Signed cursor coordinate lost");
        for (size_t size = 0; size < response.size(); ++size)
            check(!cursor_protocol::parse(response.data(), size, parsed), "Truncated cursor packet accepted");
        response[111] = 1;
        check(!cursor_protocol::parse(response.data(), response.size(), parsed),
              "Cursor reserved byte accepted");
        std::cout << "UVN1 vectors, session checks, format, bounds and malformed packets passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
