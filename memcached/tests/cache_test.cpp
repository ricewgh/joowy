#include "cache.hpp"

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
}

int main() {
    try {
        Cache cache;
        check(!cache.get("missing"), "missing get");
        check(!cache.erase("missing"), "missing delete");
        cache.set("key", {"first", 7, std::nullopt});
        auto entry = cache.get("key");
        check(entry && entry->value == "first" && entry->flags == 7, "get after set");
        cache.set("key", {"", 42, std::nullopt});
        entry = cache.get("key");
        check(entry && entry->value.empty() && entry->flags == 42, "overwrite and empty value");
        check(cache.erase("key") && !cache.get("key"), "delete");

        cache.set("expired", {"old", 0, Cache::Clock::now() - std::chrono::seconds(1)});
        check(!cache.get("expired"), "expired get");
        cache.set("expired", {"old", 0, Cache::Clock::now() - std::chrono::seconds(1)});
        check(!cache.erase("expired"), "expired delete");
        cache.set("expired", {"new", 0, std::nullopt});
        check(cache.get("expired")->value == "new", "replace expired entry");

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
                    if (!own || own->value != value || own->flags != static_cast<std::uint32_t>(id)) {
                        failed = true;
                    }
                    cache.set("shared", {shared_value, static_cast<std::uint32_t>(id), std::nullopt});
                    const auto shared = cache.get("shared");
                    if (shared && (shared->flags >= thread_count
                        || shared->value != std::string(128, static_cast<char>('a' + shared->flags)))) {
                        failed = true;
                    }
                    if (i % 3 == 0) cache.erase("shared");
                    if (!cache.erase(own_key)) failed = true;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        check(!failed, "concurrent operations");
        std::cout << "Cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cache test failed: " << error.what() << '\n';
        return 1;
    }
}
