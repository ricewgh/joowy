#ifndef JOOWY_MEMCACHED_NETWORK_HPP
#define JOOWY_MEMCACHED_NETWORK_HPP

#include <cstddef>
#include <string>
#include <string_view>

class SocketReader {
public:
    explicit SocketReader(int socket) : socket_(socket) {}

    // Returns false on EOF, I/O failure, or a line exceeding the limit.
    bool read_line(std::string& line, std::size_t limit);
    bool read_bytes(std::string& bytes, std::size_t count);

private:
    bool receive();
    int socket_;
    std::string buffer_;
};

bool send_all(int socket, const std::string& data);
bool valid_key(std::string_view key);
bool parse_port(const std::string& text, unsigned short& port);

#endif
