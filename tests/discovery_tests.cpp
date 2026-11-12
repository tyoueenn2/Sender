#include "../src/lan_discovery.hpp"
#include <atomic>
#include <iostream>
#include <thread>
using namespace lan_discovery;
namespace {
void check(bool value) {
    if (!value) throw std::runtime_error("Discovery check failed");
}
}
int main() {
    try {
        auto request = query(123);
        check(parse_query(request) == 123);
        request[5] = 1;
        check(!parse_query(request));
        request = query(0);
        check(!parse_query(request));
        check(!parse_query(std::array<uint8_t, 17>{}));
        auto response = reply(123, 5000, "0.0.0.0");
        auto parsed = parse_reply(response, 123, {"192.168.1.20", receiver_port});
        check(parsed && parsed->ip == "192.168.1.20" && parsed->port == 5000);
        check(!parse_reply(response, 124, {"192.168.1.20", receiver_port}));
        response[20] = 1;
        check(!parse_reply(response, 123, {"192.168.1.20", receiver_port}));
        response = reply(123, 0, "0.0.0.0");
        check(!parse_reply(response, 123, {"192.168.1.20", receiver_port}));
        response = reply(123, 5123, "192.168.1.21");
        parsed = parse_reply(response, 123, {"192.168.1.20", receiver_port});
        check(parsed && parsed->ip == "192.168.1.21" && parsed->port == 5123);
        response = reply(123, 5123, "255.255.255.255");
        check(!parse_reply(response, 123, {"192.168.1.20", receiver_port}));

        Socket server(0, "127.0.0.1");
        std::jthread worker([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::array<uint8_t, 64> buffer{};
                Endpoint peer;
                const auto count = server.receive(buffer, peer);
                if (count <= 0) continue;
                const auto nonce = parse_query({buffer.data(), size_t(count)});
                if (!nonce) continue;
                server.send(reply(nonce + 1, 5555, "0.0.0.0"), peer);
                server.send(std::vector<uint8_t>(4096), peer); // Oversized datagrams are ignored.
                server.send(reply(nonce, 5555, "0.0.0.0"), peer);
                server.send(reply(nonce, 5555, "0.0.0.0"), peer);
                server.send(reply(nonce, 5556, "0.0.0.0"), peer); // Multiple instances stay distinct.
            }
        });
        const auto found = find_receivers({"127.0.0.1"}, server.port(), 450);
        worker.request_stop();
        worker.join();
        check(found.size() == 2 && found[0].ip == "127.0.0.1");
        check(found[0].port == 5555 && found[1].port == 5556);
        Socket quiet(0, "127.0.0.1");
        check(find_receivers({"127.0.0.1"}, quiet.port(), 100).empty());
        std::cout << "Discovery protocol, malformed replies, retry deduplication and multiple results passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
