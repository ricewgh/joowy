#ifndef JOOWY_MEMCACHED_CACHE_HPP
#define JOOWY_MEMCACHED_CACHE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(__GLIBC__)
#include <pthread.h>
#endif

// glibc's default rwlock prefers readers, so a steady stream of readers can starve a writer
// for seconds. This lock makes new readers queue behind a waiting writer.
// On other C libraries it falls back to std::shared_mutex, which gives no such guarantee.
class WriterPreferringLock {
public:
#if defined(__GLIBC__)
    WriterPreferringLock() {
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_init(&attr);
        pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
    }
    ~WriterPreferringLock() { pthread_rwlock_destroy(&lock_); }
    void lock() { pthread_rwlock_wrlock(&lock_); }
    void unlock() { pthread_rwlock_unlock(&lock_); }
    void lock_shared() { pthread_rwlock_rdlock(&lock_); }
    void unlock_shared() { pthread_rwlock_unlock(&lock_); }

private:
    pthread_rwlock_t lock_;
#else
    void lock() { lock_.lock(); }
    void unlock() { lock_.unlock(); }
    void lock_shared() { lock_.lock_shared(); }
    void unlock_shared() { lock_.unlock_shared(); }

private:
    std::shared_mutex lock_;
#endif

public:
    WriterPreferringLock(const WriterPreferringLock&) = delete;
    WriterPreferringLock& operator=(const WriterPreferringLock&) = delete;
};

// Thread-safe string cache. The key space is split into independently locked shards, so
// operations on different shards never contend. Values are immutable and reference counted,
// so readers copy a pointer under the lock rather than the value itself.
class Cache {
public:
    // Relative TTLs use a monotonic clock so wall-clock changes cannot expire or revive entries.
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Entry(std::string v, std::uint32_t f, std::optional<Clock::time_point> e)
            : value(std::make_shared<const std::string>(std::move(v))), flags(f), expires_at(e) {}

        std::shared_ptr<const std::string> value;
        std::uint32_t flags;
        std::optional<Clock::time_point> expires_at;
    };

    // max_bytes is an approximate limit on keys, values and bookkeeping; 0 means unlimited.
    // When a shard exceeds its share, its least recently written entries are evicted.
    explicit Cache(std::size_t max_bytes = 0);

    std::optional<Entry> get(const std::string& key);
    void set(std::string key, Entry entry);
    bool erase(const std::string& key);

    // Removes expired entries from one shard per call, cycling through all shards.
    void purge_expired();

    // Number of stored entries, including expired ones not yet removed.
    std::size_t size() const;

private:
    static constexpr std::size_t shard_count = 32;

    struct Stored {
        Entry entry;
        std::list<std::string>::iterator order;
        std::size_t cost;
    };

    struct Shard {
        mutable WriterPreferringLock lock;
        std::unordered_map<std::string, Stored> map;
        std::list<std::string> order;  // least recently written first
        std::size_t bytes = 0;
    };

    using Map = std::unordered_map<std::string, Stored>;

    Shard& shard_for(const std::string& key);
    static bool expired(const Entry& entry, Clock::time_point now);
    static void remove(Shard& shard, Map::iterator it);
    void evict(Shard& shard) const;

    std::array<Shard, shard_count> shards_;
    std::size_t max_shard_bytes_;
    std::atomic<std::size_t> next_purge_{0};
};

#endif
