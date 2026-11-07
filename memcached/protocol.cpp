#include "protocol.hpp"
#include "network.hpp"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::size_t max_line = 8192;
constexpr std::size_t max_value = 1024 * 1024;
constexpr std::uint32_t thirty_days = 60 * 60 * 24 * 30;

template <typename T>
bool parse_number(const std::string& text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::vector<std::string> tokens(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> result;
    for (std::string word; input >> word;) {
        result.push_back(std::move(word));
    }
    return result;
}

bool is_noreply(const std::vector<std::string>& parts, std::size_t expected) {
    return parts.size() == expected + 1 && parts.back() == "noreply";
}
}

void serve_client(int socket, Cache& cache) {
    SocketReader reader(socket);
    std::string line;
    while (reader.read_line(line, max_line)) {
        const auto parts = tokens(line);
        if (parts.empty()) {
            if (!send_all(socket, "ERROR\r\n")) return;
        } else if (parts[0] == "get") {
            if (parts.size() < 2) {
                if (!send_all(socket, "CLIENT_ERROR bad command line format\r\n")) return;
                continue;
            }
            bool valid = true;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                if (!valid_key(parts[i])) valid = false;
            }
            if (!valid) {
                if (!send_all(socket, "CLIENT_ERROR bad key\r\n")) return;
                continue;
            }
            for (std::size_t i = 1; i < parts.size(); ++i) {
                const auto entry = cache.get(parts[i]);
                if (entry && !send_all(socket, "VALUE " + parts[i] + " "
                    + std::to_string(entry->flags) + " " + std::to_string(entry->value.size())
                    + "\r\n" + entry->value + "\r\n")) return;
            }
            if (!send_all(socket, "END\r\n")) return;
        } else if (parts[0] == "set") {
            std::uint32_t flags = 0;
            std::uint32_t expiration = 0;
            std::size_t length = 0;
            const bool noreply = is_noreply(parts, 5);
            if ((parts.size() != 5 && !noreply) || !valid_key(parts[1])
                || !parse_number(parts[2], flags) || !parse_number(parts[3], expiration)
                || !parse_number(parts[4], length)) {
                send_all(socket, "CLIENT_ERROR bad command line format\r\n");
                return;
            }
            if (length > max_value) {
                send_all(socket, "SERVER_ERROR object too large\r\n");
                return;
            }
            std::string value;
            std::string terminator;
            if (!reader.read_bytes(value, length) || !reader.read_bytes(terminator, 2)) return;
            if (terminator != "\r\n") {
                send_all(socket, "CLIENT_ERROR bad data chunk\r\n");
                return;
            }
            std::optional<Cache::Clock::time_point> expires_at;
            if (expiration != 0) {
                expires_at = expiration <= thirty_days
                    ? Cache::Clock::now() + std::chrono::seconds(expiration)
                    : Cache::Clock::time_point(std::chrono::seconds(expiration));
            }
            cache.set(parts[1], {std::move(value), flags, expires_at});
            if (!noreply && !send_all(socket, "STORED\r\n")) return;
        } else if (parts[0] == "delete") {
            const bool noreply = is_noreply(parts, 2);
            if ((parts.size() != 2 && !noreply) || !valid_key(parts[1])) {
                if (!send_all(socket, "CLIENT_ERROR bad command line format\r\n")) return;
                continue;
            }
            const bool removed = cache.erase(parts[1]);
            if (!noreply && !send_all(socket, removed ? "DELETED\r\n" : "NOT_FOUND\r\n")) return;
        } else if (parts[0] == "quit" && parts.size() == 1) {
            return;
        } else {
            if (!send_all(socket, "ERROR\r\n")) return;
        }
    }
}
