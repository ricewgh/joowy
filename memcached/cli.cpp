#include "network.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::size_t max_value = 1024 * 1024;

bool no_more_arguments(std::istringstream& input) {
    input >> std::ws;
    return input.eof();
}

bool read_response(SocketReader& reader, const std::string& command) {
    std::string line;
    if (!reader.read_line(line, 8192)) return false;
    if (command != "get" || line.rfind("VALUE ", 0) != 0) {
        std::cout << line << '\n';
        return true;
    }
    std::istringstream header(line);
    std::string marker, key, flags, length_text;
    if (!(header >> marker >> key >> flags >> length_text) || !no_more_arguments(header)) {
        std::cerr << "Invalid VALUE response\n";
        return false;
    }
    std::size_t length = 0;
    const auto parsed = std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
    if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size()
        || length > max_value) {
        std::cerr << "Invalid value length in response\n";
        return false;
    }
    std::string value, terminator, end;
    if (!reader.read_bytes(value, length) || !reader.read_bytes(terminator, 2)
        || terminator != "\r\n" || !reader.read_line(end, 8192) || end != "END") {
        std::cerr << "Invalid or incomplete VALUE response\n";
        return false;
    }
    std::cout << "VALUE " << key << " flags=" << flags << " bytes=" << length
              << " " << std::quoted(value) << "\nEND\n";
    return true;
}
}

int main(int argc, char** argv) {
    unsigned short port = 11211;
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
        std::cerr << "Usage: memcached_cli [port (1-65535)]\n";
        return 1;
    }
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "connect: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return 1;
    }
    std::cout << "Commands: get KEY, set KEY VALUE, delete KEY, quit\n"
              << "Use double quotes for values containing spaces.\n";
    SocketReader reader(socket_fd);
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command, key, value;
        input >> command;
        if (command == "quit" && no_more_arguments(input)) break;
        if ((command != "get" && command != "set" && command != "delete")
            || !(input >> std::quoted(key)) || !valid_key(key)
            || (command == "set" && !(input >> std::quoted(value)))
            || !no_more_arguments(input) || value.size() > max_value) {
            std::cerr << "Invalid command, key, or value; see command syntax above.\n";
            continue;
        }
        std::string request = command + " " + key;
        if (command == "set") {
            request += " 0 0 " + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        } else {
            request += "\r\n";
        }
        if (!send_all(socket_fd, request) || !read_response(reader, command)) {
            std::cerr << "Connection lost or invalid server response\n";
            close(socket_fd);
            return 1;
        }
    }
    close(socket_fd);
    return 0;
}
