#include "cache.hpp"

#include <mutex>
#include <utility>

std::optional<Cache::Entry> Cache::get(const std::string& key) {
    {
        std::shared_lock lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        if (!it->second.expires_at || *it->second.expires_at > Clock::now()) {
            return it->second;
        }
    }

    std::unique_lock lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    if (it->second.expires_at && *it->second.expires_at <= Clock::now()) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void Cache::set(std::string key, Entry entry) {
    std::unique_lock lock(mutex_);
    entries_.insert_or_assign(std::move(key), std::move(entry));
}

bool Cache::erase(const std::string& key) {
    std::unique_lock lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    const bool live = !it->second.expires_at || *it->second.expires_at > Clock::now();
    entries_.erase(it);
    return live;
}
