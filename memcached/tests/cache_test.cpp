#include "cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using Clock = std::chrono::steady_clock;

void test_basic_operations() {
    Cache cache;
    check(!cache.get("missing"), "missing get");
    check(!cache.erase("missing"), "missing delete");
    cache.set("key", {"first", 7, std::nullopt});
    auto entry = cache.get("key");
    check(entry && *entry->value == "first" && entry->flags == 7, "get after set");
    cache.set("key", {"", 42, std::nullopt});
    entry = cache.get("key");
    check(entry && entry->value->empty() && entry->flags == 42, "overwrite and empty value");
    check(cache.size() == 1, "overwrite keeps one entry");
    check(cache.erase("key") && !cache.get("key"), "delete");
    check(cache.size() == 0, "empty after delete");
}

void test_expiration() {
    Cache cache;
    cache.set("expired", {"old", 0, Cache::Clock::now() - std::chrono::seconds(1)});
    check(!cache.get("expired"), "expired get");
    check(cache.size() == 0, "expired get removes the entry");
    cache.set("expired", {"old", 0, Cache::Clock::now() - std::chrono::seconds(1)});
    check(!cache.erase("expired"), "expired delete");
    cache.set("expired", {"new", 0, std::nullopt});
    check(*cache.get("expired")->value == "new", "replace expired entry");

    // Entries nobody reads again are removed by the periodic purge, one shard per call.
    for (int i = 0; i < 200; ++i) {
        cache.set("stale-" + std::to_string(i), {"x", 0, Cache::Clock::now() - std::chrono::seconds(1)});
    }
    cache.set("live", {"x", 0, Cache::Clock::now() + std::chrono::hours(1)});
    check(cache.size() > 100, "stale entries present before purge");
    for (int i = 0; i < 64; ++i) cache.purge_expired();
    check(cache.size() == 2, "purge removes only expired entries");
    check(cache.get("live").has_value() && cache.get("expired").has_value(), "purge keeps live entries");
}

void test_eviction() {
    constexpr int keys = 2000;
    Cache cache(128 * 1000);  // small enough that most entries cannot be kept
    const std::string value(500, 'v');
    for (int i = 0; i < keys; ++i) {
        cache.set("key-" + std::to_string(i), {value, 0, std::nullopt});
    }
    check(cache.size() > 0 && cache.size() <= 200, "size stays near the limit");
    check(cache.get("key-" + std::to_string(keys - 1)).has_value(), "newest entry survives");
    check(!cache.get("key-0"), "oldest entry evicted");

    // Rewriting an entry refreshes its position, so it outlives entries written earlier.
    Cache small(32 * 2000);
    const std::string big(600, 'b');
    for (int i = 0; i < 1000; ++i) {
        small.set("fresh", {big, 0, std::nullopt});
        small.set("churn-" + std::to_string(i), {big, 0, std::nullopt});
    }
    check(small.get("fresh").has_value(), "frequently written entry survives");
}

void test_concurrent_mixed_operations() {
    Cache cache;
    constexpr int thread_count = 8;
    constexpr int iterations = 5000;
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int id = 0; id < thread_count; ++id) {
        threads.emplace_back([&, id] {
            const std::string own_key = "thread-" + std::to_string(id);
            const std::string shared_value(128, static_cast<char>('a' + id));
            for (int i = 0; i < iterations; ++i) {
                const std::string value = std::to_string(i);
                cache.set(own_key, {value, static_cast<std::uint32_t>(id), std::nullopt});
                const auto own = cache.get(own_key);
                if (!own || *own->value != value || own->flags != static_cast<std::uint32_t>(id)) {
                    failed = true;
                }
                cache.set("shared", {shared_value, static_cast<std::uint32_t>(id), std::nullopt});
                const auto shared = cache.get("shared");
                if (shared && (shared->flags >= thread_count
                    || *shared->value != std::string(128, static_cast<char>('a' + shared->flags)))) {
                    failed = true;
                }
                if (i % 3 == 0) cache.erase("shared");
                if (!cache.erase(own_key)) failed = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(!failed, "concurrent operations");
    check(cache.size() <= 1, "only the shared key may remain");
}

// Regression test: with a reader-preferring lock a single set stalled for seconds while
// other threads kept reading the same key.
void test_writers_not_starved_by_readers() {
    Cache cache;
    const std::string value(100 * 1024, 'x');
    cache.set("hot", {value, 0, std::nullopt});

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 6; ++i) {
        readers.emplace_back([&] {
            while (!stop) {
                const auto entry = cache.get("hot");
                if (!entry || entry->value->size() != value.size()) failed = true;
            }
        });
    }

    std::chrono::duration<double> slowest{0};
    for (int i = 0; i < 20; ++i) {
        const auto start = Clock::now();
        cache.set("hot", {value, static_cast<std::uint32_t>(i), std::nullopt});
        slowest = std::max(slowest, std::chrono::duration<double>(Clock::now() - start));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stop = true;
    for (auto& reader : readers) reader.join();
    check(!failed, "readers always see a complete value");
    check(slowest < std::chrono::seconds(1), "writer starved by readers");
}
}

int main() {
    try {
        test_basic_operations();
        test_expiration();
        test_eviction();
        test_concurrent_mixed_operations();
        test_writers_not_starved_by_readers();
        std::cout << "Cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cache test failed: " << error.what() << '\n';
        return 1;
    }
}
