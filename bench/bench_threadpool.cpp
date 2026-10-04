#include "threadpool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

struct Result {
    std::string name;
    double seconds = 0;
    std::uint64_t tasks = 0;
    std::uint64_t checksum = 0;
};

void print_result(const Result& r) {
    const double tps = r.seconds > 0.0 ? static_cast<double>(r.tasks) / r.seconds : 0.0;
    std::cout << std::left << std::setw(28) << r.name << "  time_s=" << std::fixed
              << std::setprecision(6) << r.seconds << "  tasks=" << r.tasks
              << "  tasks_per_s=" << std::setprecision(1) << tps
              << "  checksum=" << r.checksum << std::endl;
}

unsigned tiny_work(unsigned x) { return x + 1u; }

unsigned heavy_work(unsigned n) {
    // Tight integer mix so the compiler cannot trivially DCE the loop under -O2
    // unless it proves the result unused (we return it).
    unsigned acc = 0x9e3779b9u;
    for (unsigned i = 1; i <= n; ++i) {
        acc ^= i * 2654435761u;
        acc = (acc << 7) | (acc >> 25);
        acc += i;
    }
    return acc;
}

Result bench_pool_tiny(std::size_t workers, std::uint64_t n) {
    fastpool::ThreadPool pool(workers);
    std::vector<std::future<unsigned>> futs;
    futs.reserve(static_cast<std::size_t>(n));
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        futs.push_back(pool.submit([i] { return tiny_work(static_cast<unsigned>(i)); }));
    }
    std::uint64_t sum = 0;
    for (auto& f : futs) {
        sum += f.get();
    }
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    return {"threadpool tiny", sec, n, sum};
}

Result bench_async_tiny(std::uint64_t n) {
    std::vector<std::future<unsigned>> futs;
    futs.reserve(static_cast<std::size_t>(n));
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        futs.push_back(std::async(std::launch::async,
                                  [i] { return tiny_work(static_cast<unsigned>(i)); }));
    }
    std::uint64_t sum = 0;
    for (auto& f : futs) {
        sum += f.get();
    }
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    return {"std::async tiny", sec, n, sum};
}

Result bench_pool_heavy(std::size_t workers, std::uint64_t n, unsigned iters) {
    fastpool::ThreadPool pool(workers);
    std::vector<std::future<unsigned>> futs;
    futs.reserve(static_cast<std::size_t>(n));
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        futs.push_back(pool.submit([iters, i] {
            return heavy_work(iters) ^ static_cast<unsigned>(i);
        }));
    }
    std::uint64_t sum = 0;
    for (auto& f : futs) {
        sum += f.get();
    }
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    return {"threadpool heavy", sec, n, sum};
}

Result bench_async_heavy(std::uint64_t n, unsigned iters) {
    std::vector<std::future<unsigned>> futs;
    futs.reserve(static_cast<std::size_t>(n));
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        futs.push_back(std::async(std::launch::async, [iters, i] {
            return heavy_work(iters) ^ static_cast<unsigned>(i);
        }));
    }
    std::uint64_t sum = 0;
    for (auto& f : futs) {
        sum += f.get();
    }
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    return {"std::async heavy", sec, n, sum};
}

}  // namespace

int main() {
    const std::size_t workers = std::max(1u, std::thread::hardware_concurrency());
    constexpr std::uint64_t kTiny = 100000;
    constexpr std::uint64_t kHeavy = 1000;
    constexpr unsigned kHeavyIters = 250000;

    std::cout << "fastpool threadpool benchmark\n";
    std::cout << "workers=" << workers << "  hw_concurrency="
              << std::thread::hardware_concurrency() << "\n\n";

    const Result pt = bench_pool_tiny(workers, kTiny);
    print_result(pt);
    const Result at = bench_async_tiny(kTiny);
    print_result(at);

    const Result ph = bench_pool_heavy(workers, kHeavy, kHeavyIters);
    print_result(ph);
    const Result ah = bench_async_heavy(kHeavy, kHeavyIters);
    print_result(ah);

    return 0;
}
