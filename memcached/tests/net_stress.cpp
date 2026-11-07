// Network stress: N client threads, random get/set/delete, every returned value is verified
// to be a well-formed value for its key (values embed key + length, so torn/mixed data is detected).
// Usage: net_stress PORT CLIENTS SECONDS KEYS VALUE_BYTES. Exits non-zero on any error or bad value.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Conn {
    int fd = -1;
    std::string buf;
    bool open(unsigned short port, bool nodelay) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (sockaddr*)&a, sizeof a) < 0) return false;
        int one = 1;
        if (nodelay) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return true;
    }
    bool send_all(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }
    bool fill() {
        char tmp[65536];
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        return true;
    }
    bool line(std::string& out) {
        size_t e;
        while ((e = buf.find("\r\n")) == std::string::npos)
            if (!fill()) return false;
        out = buf.substr(0, e);
        buf.erase(0, e + 2);
        return true;
    }
    bool bytes(std::string& out, size_t n) {
        while (buf.size() < n)
            if (!fill()) return false;
        out = buf.substr(0, n);
        buf.erase(0, n);
        return true;
    }
};

// A value must be exactly "<key>|<version>|" followed only by 'x' padding, whichever run wrote it.
static bool well_formed(const std::string& key, const std::string& v) {
    if (v.compare(0, key.size() + 1, key + "|") != 0) return false;
    size_t i = key.size() + 1;
    size_t digits = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
    if (digits == 0 || i >= v.size() || v[i] != '|') return false;
    return v.find_first_not_of('x', i + 1) == std::string::npos;
}

static std::string value_for(const std::string& key, unsigned version, size_t size) {
    std::string v = key + "|" + std::to_string(version) + "|";
    v.resize(std::max(size, v.size()), 'x');
    return v;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "Usage: net_stress PORT CLIENTS SECONDS KEYS VALUE_BYTES\n");
        return 2;
    }
    unsigned short port = std::atoi(argv[1]);
    int clients = std::atoi(argv[2]);
    double seconds = std::atof(argv[3]);
    int keys = std::atoi(argv[4]);
    size_t vsize = std::atoi(argv[5]);

    std::atomic<bool> stop{false};
    std::atomic<long> ops{0}, errors{0}, bad_values{0}, hits{0}, misses{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> lat(clients);

    for (int c = 0; c < clients; ++c) {
        threads.emplace_back([&, c] {
            Conn conn;
            if (!conn.open(port, true)) { ++errors; return; }
            std::mt19937 rng(c * 7919 + 1);
            unsigned version = 1;
            while (!stop) {
                std::string key = "key-" + std::to_string(rng() % keys);
                int op = rng() % 10;
                auto t0 = Clock::now();
                std::string l;
                if (op < 2) {  // 20% set
                    std::string v = value_for(key, version++, vsize);
                    if (!conn.send_all("set " + key + " 0 0 " + std::to_string(v.size()) + "\r\n" + v + "\r\n")
                        || !conn.line(l) || l != "STORED") { ++errors; break; }
                } else if (op < 3) {  // 10% delete
                    if (!conn.send_all("delete " + key + "\r\n") || !conn.line(l)
                        || (l != "DELETED" && l != "NOT_FOUND")) { ++errors; break; }
                } else {  // 70% get
                    if (!conn.send_all("get " + key + "\r\n") || !conn.line(l)) { ++errors; break; }
                    if (l == "END") {
                        ++misses;
                    } else {
                        char k[300]; unsigned flags; size_t len;
                        if (sscanf(l.c_str(), "VALUE %299s %u %zu", k, &flags, &len) != 3 || key != k) { ++bad_values; break; }
                        std::string v, term, end;
                        if (!conn.bytes(v, len) || !conn.bytes(term, 2) || !conn.line(end) || end != "END") { ++errors; break; }
                        if (!well_formed(key, v) || term != "\r\n") {
                            if (bad_values++ < 3) std::fprintf(stderr, "BAD key=%s len=%zu head=%.40s\n", key.c_str(), len, v.c_str());
                        }
                        ++hits;
                    }
                }
                lat[c].push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
                ++ops;
            }
            close(conn.fd);
        });
    }
    auto start = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto& t : threads) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::vector<double> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) { return all.empty() ? 0.0 : all[std::min(all.size() - 1, (size_t)(all.size() * p))]; };
    std::printf("clients=%d keys=%d vsize=%zu | ops=%ld (%.0f/s) hits=%ld misses=%ld errors=%ld BAD=%ld | p50=%.0fus p99=%.0fus max=%.0fus\n",
                clients, keys, vsize, ops.load(), ops / elapsed, hits.load(), misses.load(), errors.load(),
                bad_values.load(), pct(0.5), pct(0.99), all.empty() ? 0.0 : all.back());
    return (errors || bad_values) ? 1 : 0;
}
