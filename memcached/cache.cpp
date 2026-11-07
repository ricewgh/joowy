#include "cache.hpp"

#include <functional>
#include <iterator>
#include <vector>

namespace {
constexpr std::size_t entry_overhead = 128;
}

Cache::Cache(std::size_t max_bytes) : max_shard_bytes_(max_bytes / shard_count) {}

Cache::Shard& Cache::shard_for(const std::string& key) {
    return shards_[std::hash<std::string>{}(key) % shard_count];
}

bool Cache::expired(const Entry& entry, Clock::time_point now) {
    return entry.expires_at && *entry.expires_at <= now;
}

void Cache::remove(Shard& shard, Map::iterator it) {
    shard.bytes -= it->second.cost;
    shard.order.erase(it->second.order);
    shard.map.erase(it);
}

void Cache::evict(Shard& shard) const {
    if (max_shard_bytes_ == 0) {
        return;
    }
    // The entry just written is last in the order and is never evicted itself.
    while (shard.bytes > max_shard_bytes_ && shard.order.size() > 1) {
        remove(shard, shard.map.find(shard.order.front()));
    }
}

std::optional<Cache::Entry> Cache::get(const std::string& key) {
    Shard& shard = shard_for(key);
    {
        std::shared_lock lock(shard.lock);
        const auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            return std::nullopt;
        }
        if (!expired(it->second.entry, Clock::now())) {
            return it->second.entry;
        }
    }

    // Re-check under the exclusive lock: the entry may have been replaced meanwhile.
    std::unique_lock lock(shard.lock);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return std::nullopt;
    }
    if (expired(it->second.entry, Clock::now())) {
        remove(shard, it);
        return std::nullopt;
    }
    return it->second.entry;
}

void Cache::set(std::string key, Entry entry) {
    const std::size_t cost = key.size() * 2 + entry.value->size() + entry_overhead;
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.lock);
    const auto it = shard.map.find(key);
    if (it != shard.map.end()) {
        shard.bytes = shard.bytes - it->second.cost + cost;
        it->second.entry = std::move(entry);
        it->second.cost = cost;
        shard.order.splice(shard.order.end(), shard.order, it->second.order);
    } else {
        shard.order.push_back(key);
        shard.bytes += cost;
        shard.map.emplace(std::move(key), Stored{std::move(entry), std::prev(shard.order.end()), cost});
    }
    evict(shard);
}

bool Cache::erase(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.lock);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    const bool live = !expired(it->second.entry, Clock::now());
    remove(shard, it);
    return live;
}

void Cache::purge_expired() {
    Shard& shard = shards_[next_purge_.fetch_add(1) % shard_count];

    // Scan under the shared lock so readers are not blocked; only removal needs exclusivity.
    std::vector<std::string> candidates;
    {
        std::shared_lock lock(shard.lock);
        const auto now = Clock::now();
        for (const auto& [key, stored] : shard.map) {
            if (expired(stored.entry, now)) {
                candidates.push_back(key);
            }
        }
    }
    if (candidates.empty()) {
        return;
    }

    std::unique_lock lock(shard.lock);
    const auto now = Clock::now();
    for (const auto& key : candidates) {
        const auto it = shard.map.find(key);
        if (it != shard.map.end() && expired(it->second.entry, now)) {
            remove(shard, it);
        }
    }
}

std::size_t Cache::size() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard.lock);
        total += shard.map.size();
    }
    return total;
}
