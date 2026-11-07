#include "network.hpp"

#include <charconv>
#include <cerrno>
#include <sys/socket.h>

bool SocketReader::receive() {
    char data[4096];
    ssize_t count;
    do {
        count = recv(socket_, data, sizeof(data), 0);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
        return false;
    }
    buffer_.append(data, static_cast<std::size_t>(count));
    return true;
}

bool SocketReader::read_line(std::string& line, std::size_t limit) {
    while (true) {
        const auto end = buffer_.find("\r\n");
        if (end != std::string::npos) {
            if (end > limit) {
                return false;
            }
            line = buffer_.substr(0, end);
            buffer_.erase(0, end + 2);
            return true;
        }
        if (buffer_.size() > limit + 1 || !receive()) {
            return false;
        }
    }
}

bool SocketReader::read_bytes(std::string& bytes, std::size_t count) {
    while (buffer_.size() < count) {
        if (!receive()) {
            return false;
        }
    }
    bytes.assign(buffer_.data(), count);
    buffer_.erase(0, count);
    return true;
}

bool send_all(int socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t count;
        do {
            count = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool valid_key(const std::string& key) {
    if (key.empty() || key.size() > 250) {
        return false;
    }
    for (unsigned char ch : key) {
        if (ch <= 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool parse_port(const std::string& text, unsigned short& port) {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<unsigned short>(value);
    return true;
}
