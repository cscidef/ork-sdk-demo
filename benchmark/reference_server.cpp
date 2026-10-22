#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
std::atomic<bool> running{true};

void on_signal(int) {
    // Benchmark orchestration owns this process; terminate immediately so a
    // blocking accept()/recvfrom() cannot delay switching implementations.
    std::_Exit(0);
}

bool read_exact(int fd, void* buffer, size_t size) {
    auto* out = static_cast<unsigned char*>(buffer);
    while (size > 0) {
        const ssize_t n = recv(fd, out, size, 0);
        if (n <= 0) return false;
        out += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool write_exact(int fd, const void* buffer, size_t size) {
    const auto* in = static_cast<const unsigned char*>(buffer);
    while (size > 0) {
        const ssize_t n = send(fd, in, size, 0);
        if (n <= 0) return false;
        in += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void handle_tcp(int fd) {
    while (running) {
        uint32_t network_size = 0;
        if (!read_exact(fd, &network_size, sizeof(network_size))) break;
        const uint32_t size = ntohl(network_size);
        if (size == 0 || size > 4 * 1024 * 1024) break;
        std::vector<unsigned char> payload(size);
        if (!read_exact(fd, payload.data(), payload.size())) break;
        if (!write_exact(fd, &network_size, sizeof(network_size)) ||
            !write_exact(fd, payload.data(), payload.size())) break;
    }
    close(fd);
}

int bind_socket(int type, uint16_t port) {
    const int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void tcp_loop(uint16_t port) {
    const int server = bind_socket(SOCK_STREAM, port);
    if (server < 0 || listen(server, 256) != 0) {
        std::cerr << "TCP bind/listen failed: " << std::strerror(errno) << '\n';
        running = false;
        return;
    }
    while (running) {
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread(handle_tcp, client).detach();
    }
    close(server);
}

void udp_loop(uint16_t port) {
    const int fd = bind_socket(SOCK_DGRAM, port);
    if (fd < 0) {
        std::cerr << "UDP bind failed: " << std::strerror(errno) << '\n';
        running = false;
        return;
    }
    std::vector<unsigned char> payload(65535);
    while (running) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const ssize_t n = recvfrom(fd, payload.data(), payload.size(), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        sendto(fd, payload.data(), static_cast<size_t>(n), 0,
               reinterpret_cast<sockaddr*>(&peer), peer_size);
    }
    close(fd);
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 38664;
    if (argc == 3 && std::string(argv[1]) == "--port") {
        const int parsed = std::stoi(argv[2]);
        if (parsed < 1 || parsed > 65535) return 2;
        port = static_cast<uint16_t>(parsed);
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "reference_server listening TCP+UDP on " << port << std::endl;
    std::thread tcp(tcp_loop, port);
    std::thread udp(udp_loop, port);
    tcp.join();
    udp.join();
    return 0;
}
