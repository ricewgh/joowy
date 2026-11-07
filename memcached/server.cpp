#include "cache.hpp"
#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr unsigned int max_clients = 256;
}

int main(int argc, char** argv) {
    unsigned short port = 11211;
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
        std::cerr << "Usage: memcached_server [port (1-65535)]\n";
        return 1;
    }

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return 1;
    }
    const int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt: " << std::strerror(errno) << '\n';
        close(listener);
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0
        || listen(listener, 128) < 0) {
        std::cerr << "bind/listen: " << std::strerror(errno) << '\n';
        close(listener);
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:" << port << std::endl;
    auto cache = std::make_shared<Cache>();
    auto clients = std::make_shared<std::atomic<unsigned int>>(0);
    while (true) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            std::cerr << "accept: " << std::strerror(errno) << '\n';
            close(listener);
            return 1;
        }
        if (clients->load() >= max_clients) {
            if (!send_all(client, "SERVER_ERROR too many connections\r\n")) {
                std::cerr << "Could not report connection limit to client\n";
            }
            close(client);
            continue;
        }
        ++*clients;
        try {
            std::thread([client, cache, clients] {
                try {
                    serve_client(client, *cache);
                } catch (const std::exception& error) {
                    std::cerr << "Client handler failed: " << error.what() << '\n';
                }
                close(client);
                --*clients;
            }).detach();
        } catch (const std::exception& error) {
            std::cerr << "Could not start client handler: " << error.what() << '\n';
            --*clients;
            close(client);
        }
    }
}
