// Cache stress: integrity (value must always match key+flags), throughput, and writer starvation.
// Usage: cache_stress [seconds per scenario, default 2]. Exits non-zero if a corrupt value is seen.
#include "cache.hpp"

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

static std::string make_value(const std::string& key, std::uint32_t version, std::size_t size) {
    std::string v = key + ":" + std::to_string(version) + ":";
    while (v.size() < size) v += v;
    v.resize(size);
    return v;
}

struct Result {
    double ops_per_sec;
    double write_p50_us, write_p99_us, write_max_us;
    long corrupt;
};

static Result run(int readers, int writers, int keys, std::size_t vsize, double seconds, bool with_ttl) {
    Cache cache;
    for (int k = 0; k < keys; ++k) {
        std::string key = "key-" + std::to_string(k);
        cache.set(key, {make_value(key, 0, vsize), 0, std::nullopt});
    }
    std::atomic<bool> stop{false};
    std::atomic<long> ops{0}, corrupt{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> write_lat(writers);

    for (int r = 0; r < readers; ++r) {
        threads.emplace_back([&, r] {
            std::mt19937 rng(r + 1);
            long local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string key = "key-" + std::to_string(rng() % keys);
                auto e = cache.get(key);
                if (e) {
                    // value must be a full, untorn value belonging to this key
                    if (e->value->size() != vsize || e->value->compare(0, key.size() + 1, key + ":") != 0) ++corrupt;
                }
                ++local;
            }
            ops += local;
        });
    }
    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            std::mt19937 rng(1000 + w);
            long local = 0;
            std::uint32_t version = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string key = "key-" + std::to_string(rng() % keys);
                std::string value = make_value(key, version++, vsize);
                std::optional<Cache::Clock::time_point> exp;
                if (with_ttl) exp = Cache::Clock::now() + std::chrono::milliseconds(rng() % 5);
                auto t0 = Clock::now();
                cache.set(key, {std::move(value), 0, exp});
                auto t1 = Clock::now();
                write_lat[w].push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                if (rng() % 4 == 0) cache.erase(key);
                ++local;
            }
            ops += local;
        });
    }
    auto start = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto& t : threads) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<double> all;
    for (auto& v : write_lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    Result res{ops / elapsed, 0, 0, 0, corrupt.load()};
    if (!all.empty()) {
        res.write_p50_us = all[all.size() / 2];
        res.write_p99_us = all[all.size() * 99 / 100];
        res.write_max_us = all.back();
    }
    return res;
}

int main(int argc, char** argv) {
    double secs = argc > 1 ? std::atof(argv[1]) : 2.0;
    struct Case { const char* name; int r, w, keys; std::size_t vs; bool ttl; };
    const Case cases[] = {
        {"1 reader,  0 writers, 1024 keys, 64B   ", 1, 0, 1024, 64, false},
        {"4 readers, 0 writers, 1024 keys, 64B   ", 4, 0, 1024, 64, false},
        {"8 readers, 0 writers, 1024 keys, 64B   ", 8, 0, 1024, 64, false},
        {"0 readers, 4 writers, 1024 keys, 64B   ", 0, 4, 1024, 64, false},
        {"7 readers, 1 writer,  1024 keys, 64B   ", 7, 1, 1024, 64, false},
        {"7 readers, 1 writer,  1 hot key, 64B   ", 7, 1, 1, 64, false},
        {"7 readers, 1 writer,  1024 keys, 100KB ", 7, 1, 1024, 100 * 1024, false},
        {"8 readers, 0 writers, 1024 keys, 100KB ", 8, 0, 1024, 100 * 1024, false},
        {"6 readers, 2 writers, 1024 keys, 64B   ", 6, 2, 1024, 64, false},
        {"6 readers, 2 writers, short TTL churn  ", 6, 2, 1024, 64, true},
    };
    std::printf("%-42s %12s %9s %9s %10s %8s\n", "scenario", "ops/sec", "w p50us", "w p99us", "w max us", "corrupt");
    long total_corrupt = 0;
    for (const auto& c : cases) {
        Result r = run(c.r, c.w, c.keys, c.vs, secs, c.ttl);
        std::printf("%-42s %12.0f %9.1f %9.1f %10.1f %8ld\n", c.name, r.ops_per_sec, r.write_p50_us,
                    r.write_p99_us, r.write_max_us, r.corrupt);
        total_corrupt += r.corrupt;
    }
    return total_corrupt ? 1 : 0;
}
