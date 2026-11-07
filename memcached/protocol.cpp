#include "protocol.hpp"
#include "network.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t max_line = 8192;
constexpr std::size_t max_value = 1024 * 1024;
constexpr std::size_t max_get_keys = 100;
constexpr std::uint32_t thirty_days = 60 * 60 * 24 * 30;

template <typename T>
bool parse_number(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) parts.push_back(line.substr(start, i - start));
    }
    return parts;
}

bool is_noreply(const std::vector<std::string_view>& parts, std::size_t expected) {
    return parts.size() == expected + 1 && parts.back() == "noreply";
}

// Zero means never; up to 30 days is relative; anything larger is a Unix timestamp.
std::optional<Cache::Clock::time_point> expiry_from(std::uint32_t expiration) {
    if (expiration == 0) {
        return std::nullopt;
    }
    const auto now = Cache::Clock::now();
    if (expiration <= thirty_days) {
        return now + std::chrono::seconds(expiration);
    }
    const auto unix_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    const auto remaining = std::chrono::seconds(expiration) - unix_now;
    return now + std::max(remaining, std::chrono::seconds(0));
}

void append_value(std::string& output, std::string_view key, const Cache::Entry& entry) {
    output += "VALUE ";
    output += key;
    output += ' ';
    output += std::to_string(entry.flags);
    output += ' ';
    output += std::to_string(entry.value->size());
    output += "\r\n";
    output += *entry.value;
    output += "\r\n";
}
}

ProcessStatus process_commands(Cache& cache, std::string& input, std::string& output,
                               std::size_t output_limit) {
    std::size_t pos = 0;
    ProcessStatus status = ProcessStatus::NeedInput;
    while (pos < input.size()) {
        if (output.size() >= output_limit) {
            status = ProcessStatus::OutputFull;
            break;
        }
        const std::size_t eol = input.find("\r\n", pos);
        if (eol == std::string::npos) {
            if (input.size() - pos > max_line) status = ProcessStatus::Close;
            break;
        }
        if (eol - pos > max_line) {
            status = ProcessStatus::Close;
            break;
        }
        const auto parts = split(std::string_view(input).substr(pos, eol - pos));
        const std::size_t next = eol + 2;

        if (parts.empty()) {
            output += "ERROR\r\n";
            pos = next;
        } else if (parts[0] == "get") {
            if (parts.size() < 2) {
                output += "CLIENT_ERROR bad command line format\r\n";
            } else if (parts.size() - 1 > max_get_keys) {
                output += "CLIENT_ERROR too many keys\r\n";
            } else if (!std::all_of(parts.begin() + 1, parts.end(), valid_key)) {
                output += "CLIENT_ERROR bad key\r\n";
            } else {
                for (std::size_t i = 1; i < parts.size(); ++i) {
                    const auto entry = cache.get(std::string(parts[i]));
                    if (entry) append_value(output, parts[i], *entry);
                }
                output += "END\r\n";
            }
            pos = next;
        } else if (parts[0] == "set") {
            std::uint32_t flags = 0;
            std::uint32_t expiration = 0;
            std::size_t length = 0;
            const bool noreply = is_noreply(parts, 5);
            if ((parts.size() != 5 && !noreply) || !valid_key(parts[1])
                || !parse_number(parts[2], flags) || !parse_number(parts[3], expiration)
                || !parse_number(parts[4], length)) {
                output += "CLIENT_ERROR bad command line format\r\n";
                status = ProcessStatus::Close;
                break;
            }
            if (length > max_value) {
                output += "SERVER_ERROR object too large\r\n";
                status = ProcessStatus::Close;
                break;
            }
            if (input.size() - next < length + 2) {
                break;  // the data block has not fully arrived
            }
            if (input.compare(next + length, 2, "\r\n") != 0) {
                output += "CLIENT_ERROR bad data chunk\r\n";
                status = ProcessStatus::Close;
                break;
            }
            cache.set(std::string(parts[1]),
                      Cache::Entry(input.substr(next, length), flags, expiry_from(expiration)));
            if (!noreply) output += "STORED\r\n";
            pos = next + length + 2;
        } else if (parts[0] == "delete") {
            const bool noreply = is_noreply(parts, 2);
            if ((parts.size() != 2 && !noreply) || !valid_key(parts[1])) {
                output += "CLIENT_ERROR bad command line format\r\n";
            } else {
                const bool removed = cache.erase(std::string(parts[1]));
                if (!noreply) output += removed ? "DELETED\r\n" : "NOT_FOUND\r\n";
            }
            pos = next;
        } else if (parts[0] == "quit" && parts.size() == 1) {
            pos = next;
            status = ProcessStatus::Close;
            break;
        } else {
            output += "ERROR\r\n";
            pos = next;
        }
    }
    input.erase(0, pos);
    return status;
}
