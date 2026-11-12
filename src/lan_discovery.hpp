#pragma once
// Kept identical to Sender/src/lan_discovery.hpp so both repositories build independently.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace lan_discovery {
constexpr uint16_t receiver_port = 39570;
struct Endpoint {
    std::string ip;
    uint16_t port = 0;
    bool ready = true;
};
inline uint64_t token() {
    std::random_device random;
    uint64_t value = 0;
    while (!value)
        value = uint64_t(random()) << 32 | random();
    return value;
}
inline uint64_t get64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = value << 8 | p[i];
    return value;
}
inline void put64(uint8_t* p, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        p[i] = uint8_t(value);
        value >>= 8;
    }
}
inline std::array<uint8_t, 16> query(uint64_t nonce) {
    std::array<uint8_t, 16> p{};
    std::memcpy(p.data(), "UVDQ", 4);
    p[4] = 1;
    put64(p.data() + 8, nonce);
    return p;
}
inline uint64_t parse_query(std::span<const uint8_t> p) {
    if (p.size() != 16 || std::memcmp(p.data(), "UVDQ", 4) || p[4] != 1 || p[5] || p[6] || p[7])
        return 0;
    return get64(p.data() + 8);
}
inline std::array<uint8_t, 24> reply(uint64_t nonce, uint16_t port, const std::string& bind_ip) {
    std::array<uint8_t, 24> p{};
    std::memcpy(p.data(), "UVDA", 4);
    p[4] = 1;
    p[6] = uint8_t(port >> 8);
    p[7] = uint8_t(port);
    put64(p.data() + 8, nonce);
    in_addr ip{};
    if (inet_pton(AF_INET, bind_ip.c_str(), &ip) != 1)
        throw std::runtime_error("Invalid Receiver bind IPv4 address");
    std::memcpy(p.data() + 16, &ip.s_addr, 4);
    return p;
}
inline std::optional<Endpoint> parse_reply(std::span<const uint8_t> p, uint64_t nonce, Endpoint from) {
    if (p.size() != 24 || std::memcmp(p.data(), "UVDA", 4) || p[4] != 1 || p[5] || !nonce ||
        get64(p.data() + 8) != nonce || p[20] || p[21] || p[22] || p[23])
        return {};
    from.port = uint16_t(uint16_t(p[6]) << 8 | p[7]);
    if (!from.port)
        return {};
    uint32_t advertised = uint32_t(p[16]) << 24 | uint32_t(p[17]) << 16 | uint32_t(p[18]) << 8 | p[19];
    if (advertised) {
        if (advertised >= 0xe0000000u || !(advertised >> 24))
            return {};
        from.ip = std::to_string(p[16]) + "." + std::to_string(p[17]) + "." +
                  std::to_string(p[18]) + "." + std::to_string(p[19]);
    }
    return from;
}
class Socket {
#ifdef _WIN32
    using Native = SOCKET;
    static void initialize() {
        struct Winsock {
            Winsock() {
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data))
                    throw std::runtime_error("Cannot initialize discovery networking");
            }
            ~Winsock() { WSACleanup(); }
        };
        static Winsock winsock;
    }
#else
    using Native = int;
    static void initialize() {}
#endif
    Native socket_ = Native(-1);

  public:
    explicit Socket(uint16_t port = 0, const std::string& bind_ip = "0.0.0.0") {
        initialize();
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        if (inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) != 1)
            throw std::runtime_error("Invalid discovery bind address");
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == Native(-1))
            throw std::runtime_error("Cannot create discovery socket");
        const int broadcast = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                       sizeof(broadcast)) || bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
            close();
            throw std::runtime_error("Cannot bind discovery socket (UDP port may already be in use)");
        }
    }
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void close() {
        if (socket_ == Native(-1))
            return;
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = Native(-1);
    }
    uint16_t port() const {
        sockaddr_in local{};
#ifdef _WIN32
        int length = sizeof(local);
#else
        socklen_t length = sizeof(local);
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length))
            throw std::runtime_error("Cannot read discovery socket address");
        return ntohs(local.sin_port);
    }
    bool send(std::span<const uint8_t> packet, const Endpoint& to) {
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(to.port);
        if (inet_pton(AF_INET, to.ip.c_str(), &peer.sin_addr) != 1)
            return false;
        return sendto(socket_, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                      reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == int(packet.size());
    }
    int receive(std::span<uint8_t> packet, Endpoint& from, int timeout_ms = 40) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval timeout{timeout_ms / 1000, timeout_ms % 1000 * 1000};
        const int ready = select(int(socket_ + 1), &readable, nullptr, nullptr, &timeout);
        if (ready < 0)
            throw std::runtime_error("Discovery receive failed");
        if (!ready)
            return 0;
        sockaddr_in peer{};
#ifdef _WIN32
        int length = sizeof(peer);
        const int count = recvfrom(socket_, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &length);
        if (count < 0 && WSAGetLastError() == WSAEMSGSIZE)
            return 0;
#else
        socklen_t length = sizeof(peer);
        const int count = int(recvfrom(socket_, packet.data(), packet.size(), MSG_TRUNC,
                                       reinterpret_cast<sockaddr*>(&peer), &length));
        if (count > int(packet.size()))
            return 0;
#endif
        if (count < 0)
            throw std::runtime_error("Discovery receive failed");
        std::array<char, 16> ip{};
        if (!inet_ntop(AF_INET, &peer.sin_addr, ip.data(), ip.size()))
            return 0;
        from = {ip.data(), ntohs(peer.sin_port)};
        return count;
    }
};
inline std::vector<std::string> broadcast_targets() {
    // Loopback also allows a Sender and Receiver on the same PC to find one another.
    std::vector<std::string> targets{"255.255.255.255", "127.0.0.1"};
#ifdef _WIN32
    ULONG size = 16384;
    std::vector<uint8_t> storage(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        storage.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                  GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &size);
    }
    if (result == NO_ERROR)
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()); adapter;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
                if (!entry->Address.lpSockaddr || entry->Address.lpSockaddr->sa_family != AF_INET ||
                    !entry->OnLinkPrefixLength || entry->OnLinkPrefixLength > 30)
                    continue;
                const auto* address = reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr);
                in_addr broadcast{};
                broadcast.s_addr = htonl(ntohl(address->sin_addr.s_addr) |
                                         (0xffffffffu >> entry->OnLinkPrefixLength));
                std::array<char, 16> ip{};
                if (inet_ntop(AF_INET, &broadcast, ip.data(), ip.size()))
                    targets.emplace_back(ip.data());
            }
        }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || !entry->ifa_broadaddr || entry->ifa_addr->sa_family != AF_INET ||
                !(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_BROADCAST))
                continue;
            const auto* broadcast = reinterpret_cast<sockaddr_in*>(entry->ifa_broadaddr);
            std::array<char, 16> ip{};
            if (inet_ntop(AF_INET, &broadcast->sin_addr, ip.data(), ip.size()))
                targets.emplace_back(ip.data());
        }
        freeifaddrs(interfaces);
    }
#endif
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.size() > 64)
        targets.resize(64);
    return targets;
}
template <class Parse>
std::vector<Endpoint> scan(std::span<const uint8_t> request, uint16_t port, Parse parse,
                           std::vector<std::string> targets = {}, int timeout_ms = 1600) {
    if (!port || timeout_ms < 1 || timeout_ms > 5000)
        throw std::runtime_error("Invalid discovery port or timeout");
    Socket socket;
    if (targets.empty())
        targets = broadcast_targets();
    std::vector<Endpoint> found;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto next_probe = std::chrono::steady_clock::now();
    bool sent = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_probe) {
            for (const auto& ip : targets)
                sent = socket.send(request, {ip, port}) || sent;
            next_probe += std::chrono::milliseconds(300);
        }
        std::array<uint8_t, 2048> buffer{};
        Endpoint from;
        const int count = socket.receive(buffer, from);
        if (count <= 0)
            continue;
        auto match = parse(std::span<const uint8_t>(buffer.data(), size_t(count)), from);
        if (match && found.size() < 32 &&
            std::none_of(found.begin(), found.end(), [&](const auto& entry) {
                return entry.ip == match->ip && entry.port == match->port;
            }))
            found.push_back(*match);
    }
    if (!sent)
        throw std::runtime_error("Could not send LAN discovery requests. Check your network connection.");
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.ip == b.ip ? a.port < b.port : a.ip < b.ip;
    });
    return found;
}
inline std::vector<Endpoint> find_receivers(std::vector<std::string> targets = {},
                                           uint16_t port = receiver_port, int timeout_ms = 1600) {
    const auto nonce = token();
    return scan(query(nonce), port,
                [nonce](auto packet, auto from) { return parse_reply(packet, nonce, from); },
                std::move(targets), timeout_ms);
}
} // namespace lan_discovery
