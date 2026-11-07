#include "cache.hpp"
#include "network.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t read_chunk = 64 * 1024;
// Command processing pauses once this much output is waiting, so a client that does not read
// its responses cannot make the server buffer an unbounded amount.
constexpr std::size_t output_limit = 1024 * 1024;
constexpr std::size_t default_cache_megabytes = 256;
constexpr unsigned default_idle_seconds = 60;
constexpr std::size_t max_connection_cap = 4096;
constexpr std::size_t reserved_descriptors = 64;

struct Limits {
    std::size_t max_connections;
    std::chrono::seconds idle_timeout;
};

struct Connection {
    int fd = -1;
    std::string input;
    std::string output;
    std::size_t sent = 0;
    bool eof = false;
    bool closing = false;
    bool more_input = false;  // unprocessed input remains because output hit the limit
    std::uint32_t interest = EPOLLIN;
    Clock::time_point last_active;

    bool pending() const { return sent < output.size(); }
    bool wants_read() const { return !eof && !closing && !more_input && !pending(); }
};

// Each worker owns an epoll instance and every connection it accepted, so connection state
// is touched by one thread only. The cache is the sole shared state.
class Worker {
public:
    Worker(int listener, Cache& cache, std::atomic<std::size_t>& active, Limits limits)
        : listener_(listener), cache_(cache), active_(active), limits_(limits) {
        epoll_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_ < 0) {
            throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));
        }
        // EPOLLEXCLUSIVE wakes one worker per incoming connection instead of all of them.
        epoll_event event{};
        event.events = EPOLLIN | EPOLLEXCLUSIVE;
        event.data.ptr = nullptr;
        if (epoll_ctl(epoll_, EPOLL_CTL_ADD, listener_, &event) < 0) {
            const int error = errno;
            close(epoll_);
            throw std::runtime_error(std::string("epoll_ctl: ") + std::strerror(error));
        }
    }

    ~Worker() {
        for (const auto& entry : connections_) {
            close(entry.second->fd);
            --active_;
        }
        close(epoll_);
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    [[noreturn]] void run() {
        epoll_event events[64];
        auto last_sweep = Clock::now();
        while (true) {
            const int count = epoll_wait(epoll_, events, 64, 1000);
            if (count < 0 && errno != EINTR) {
                throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
            }
            const auto now = Clock::now();
            for (int i = 0; i < count; ++i) {
                if (events[i].data.ptr == nullptr) {
                    accept_one(now);
                } else {
                    handle(*static_cast<Connection*>(events[i].data.ptr), events[i].events, now);
                }
            }
            if (now - last_sweep >= std::chrono::seconds(1)) {
                close_idle(now);
                last_sweep = now;
            }
        }
    }

private:
    void accept_one(Clock::time_point now) {
        const int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                std::cerr << "accept: out of file descriptors\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return;
        }
        if (active_.fetch_add(1) >= limits_.max_connections) {
            --active_;
            static const char message[] = "SERVER_ERROR too many connections\r\n";
            static_cast<void>(send(fd, message, sizeof(message) - 1, MSG_NOSIGNAL));
            close(fd);
            return;
        }
        // Responses are small and latency sensitive; do not let Nagle batch them.
        const int one = 1;
        static_cast<void>(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));

        auto connection = std::make_unique<Connection>();
        Connection* raw = connection.get();
        raw->fd = fd;
        raw->last_active = now;
        epoll_event event{};
        event.events = raw->interest;
        event.data.ptr = raw;
        if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &event) < 0) {
            --active_;
            close(fd);
            return;
        }
        connections_.emplace(raw, std::move(connection));
    }

    void handle(Connection& connection, std::uint32_t events, Clock::time_point now) {
        if (events & EPOLLERR) {
            destroy(connection);
            return;
        }
        if ((events & (EPOLLIN | EPOLLHUP)) && connection.wants_read()) {
            char buffer[read_chunk];
            const ssize_t count = recv(connection.fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                connection.input.append(buffer, static_cast<std::size_t>(count));
                connection.last_active = now;
            } else if (count == 0) {
                connection.eof = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                destroy(connection);
                return;
            }
        }
        advance(connection, now);
    }

    // Runs buffered commands and sends their responses until the connection must wait for
    // the peer (more input or output space), or is finished.
    void advance(Connection& connection, Clock::time_point now) {
        while (true) {
            if (!connection.pending() && !connection.closing) {
                const auto status = process_commands(cache_, connection.input, connection.output,
                                                     output_limit);
                connection.more_input = status == ProcessStatus::OutputFull;
                if (status == ProcessStatus::Close) connection.closing = true;
            }
            if (!flush(connection, now)) {
                destroy(connection);
                return;
            }
            if (connection.pending() || !connection.more_input || connection.closing) break;
        }
        if (!connection.pending() && (connection.closing || connection.eof)) {
            destroy(connection);
            return;
        }
        update_interest(connection);
    }

    // Returns false if the connection failed.
    bool flush(Connection& connection, Clock::time_point now) {
        while (connection.pending()) {
            const ssize_t count = send(connection.fd, connection.output.data() + connection.sent,
                                       connection.output.size() - connection.sent, MSG_NOSIGNAL);
            if (count > 0) {
                connection.sent += static_cast<std::size_t>(count);
                connection.last_active = now;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        connection.output.clear();
        connection.sent = 0;
        return true;
    }

    void update_interest(Connection& connection) {
        std::uint32_t wanted = 0;
        if (connection.wants_read()) wanted |= EPOLLIN;
        if (connection.pending()) wanted |= EPOLLOUT;
        if (wanted == connection.interest) {
            return;
        }
        epoll_event event{};
        event.events = wanted;
        event.data.ptr = &connection;
        if (epoll_ctl(epoll_, EPOLL_CTL_MOD, connection.fd, &event) < 0) {
            destroy(connection);
            return;
        }
        connection.interest = wanted;
    }

    // Connections that made no progress for the idle timeout are dropped. This also covers
    // peers that stop reading their responses.
    void close_idle(Clock::time_point now) {
        std::vector<Connection*> idle;
        for (const auto& entry : connections_) {
            if (now - entry.second->last_active > limits_.idle_timeout) {
                idle.push_back(entry.first);
            }
        }
        for (Connection* connection : idle) {
            destroy(*connection);
        }
    }

    // The connection must not be used after this call.
    void destroy(Connection& connection) {
        close(connection.fd);
        --active_;
        connections_.erase(&connection);
    }

    int listener_;
    int epoll_ = -1;
    Cache& cache_;
    std::atomic<std::size_t>& active_;
    Limits limits_;
    std::unordered_map<Connection*, std::unique_ptr<Connection>> connections_;
};

template <typename T>
bool parse_unsigned(const char* text, T& value) {
    const std::string_view view(text);
    const auto result = std::from_chars(view.data(), view.data() + view.size(), value);
    return !view.empty() && result.ec == std::errc{} && result.ptr == view.data() + view.size();
}

int usage() {
    std::cerr << "Usage: memcached_server [port (1-65535)] [cache megabytes, 0 = unlimited]"
                 " [idle timeout seconds (>= 1)]\n";
    return 1;
}

std::size_t connection_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        static_cast<void>(setrlimit(RLIMIT_NOFILE, &limit));
        getrlimit(RLIMIT_NOFILE, &limit);
        if (limit.rlim_cur > reserved_descriptors) {
            return static_cast<std::size_t>(
                std::min<rlim_t>(max_connection_cap, limit.rlim_cur - reserved_descriptors));
        }
    }
    return 1;
}
}

int main(int argc, char** argv) {
    unsigned short port = 11211;
    std::size_t megabytes = default_cache_megabytes;
    unsigned idle_seconds = default_idle_seconds;
    if (argc > 4 || (argc > 1 && !parse_port(argv[1], port))
        || (argc > 2 && !parse_unsigned(argv[2], megabytes))
        || (argc > 3 && (!parse_unsigned(argv[3], idle_seconds) || idle_seconds == 0))
        || megabytes > (std::numeric_limits<std::size_t>::max() >> 20)) {
        return usage();
    }

    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
        || listen(listener, 512) < 0) {
        std::cerr << "bind/listen: " << std::strerror(errno) << '\n';
        close(listener);
        return 1;
    }

    Cache cache(megabytes << 20);
    std::atomic<std::size_t> active{0};
    const Limits limits{connection_limit(), std::chrono::seconds(idle_seconds)};
    const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);

    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            try {
                Worker(listener, cache, active, limits).run();
            } catch (const std::exception& error) {
                std::cerr << "Worker failed: " << error.what() << '\n';
                std::exit(1);
            }
        });
    }
    std::cout << "Listening on 127.0.0.1:" << port << std::endl;

    // Expired entries nobody reads again would otherwise stay in memory.
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        cache.purge_expired();
    }
}
