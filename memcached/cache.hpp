#ifndef JOOWY_MEMCACHED_CACHE_HPP
#define JOOWY_MEMCACHED_CACHE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class Cache {
public:
    using Clock = std::chrono::system_clock;

    struct Entry {
        std::string value;
        std::uint32_t flags;
        std::optional<Clock::time_point> expires_at;
    };

    std::optional<Entry> get(const std::string& key);
    void set(std::string key, Entry entry);
    bool erase(const std::string& key);

private:
    std::unordered_map<std::string, Entry> entries_;
    mutable std::shared_mutex mutex_;
};

#endif
