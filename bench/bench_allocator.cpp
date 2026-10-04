#include "pool_allocator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

struct Result {
    std::string name;
    double seconds = 0;
    std::uint64_t ops = 0;
};

void print_result(const Result& r) {
    const double ops_s = r.seconds > 0.0 ? static_cast<double>(r.ops) / r.seconds : 0.0;
    std::cout << std::left << std::setw(36) << r.name << "  time_s=" << std::fixed
              << std::setprecision(6) << r.seconds << "  ops=" << r.ops
              << "  alloc_free_per_s=" << std::setprecision(1) << ops_s << std::endl;
}

struct alignas(64) Obj64 {
    std::uint64_t a, b, c, d, e, f, g, h;
};

static_assert(sizeof(Obj64) == 64);

constexpr std::uint64_t kOps = 1000000;

template <class AllocFn, class FreeFn>
double run_serial(std::uint64_t n, AllocFn alloc, FreeFn free_fn) {
    const auto t0 = clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        void* p = alloc();
        static_cast<Obj64*>(p)->a = i;
        free_fn(p);
    }
    return std::chrono::duration<double>(clock::now() - t0).count();
}

template <class AllocFn, class FreeFn>
double run_burst(std::uint64_t n, std::size_t burst, AllocFn alloc, FreeFn free_fn) {
    std::vector<void*> hold(burst);
    const auto t0 = clock::now();
    std::uint64_t done = 0;
    while (done < n) {
        const std::size_t batch =
            static_cast<std::size_t>(std::min<std::uint64_t>(burst, n - done));
        for (std::size_t i = 0; i < batch; ++i) {
            hold[i] = alloc();
            static_cast<Obj64*>(hold[i])->a = done + i;
        }
        for (std::size_t i = 0; i < batch; ++i) {
            free_fn(hold[i]);
        }
        done += batch;
    }
    return std::chrono::duration<double>(clock::now() - t0).count();
}

void threaded(int nthreads, std::uint64_t total_ops, auto&& per_thread) {
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(nthreads));
    const std::uint64_t per = total_ops / static_cast<std::uint64_t>(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        const std::uint64_t n =
            (t == nthreads - 1) ? (total_ops - per * static_cast<std::uint64_t>(t)) : per;
        ts.emplace_back([n, &per_thread] { per_thread(n); });
    }
    for (auto& th : ts) {
        th.join();
    }
}

}  // namespace

int main() {
    std::cout << "fastpool allocator benchmark  (64-byte objects, " << kOps
              << " alloc+free pairs)\n\n"
              << std::flush;

    {
        fastpool::PoolAllocator<sizeof(Obj64), alignof(Obj64)> pool(4096);
        const double s = run_serial(kOps, [&] { return pool.allocate(); },
                                    [&](void* p) { pool.deallocate(p); });
        print_result({"pool serial (LIFO reuse)", s, kOps});
    }
    {
        const double s = run_serial(
            kOps, [] { return ::operator new(sizeof(Obj64)); },
            [](void* p) { ::operator delete(p); });
        print_result({"new/delete serial", s, kOps});
    }

    constexpr std::size_t kBurst = 4096;
    {
        fastpool::PoolAllocator<sizeof(Obj64), alignof(Obj64)> pool(4096);
        const double s = run_burst(kOps, kBurst, [&] { return pool.allocate(); },
                                   [&](void* p) { pool.deallocate(p); });
        print_result({"pool serial burst-4k", s, kOps});
    }
    {
        const double s = run_burst(
            kOps, kBurst, [] { return ::operator new(sizeof(Obj64)); },
            [](void* p) { ::operator delete(p); });
        print_result({"new/delete serial burst-4k", s, kOps});
    }

    constexpr int kThreads = 8;
    std::cout << "\n" << kThreads << "-thread\n";

    {
        fastpool::SynchronizedPoolAllocator<sizeof(Obj64), alignof(Obj64)> pool(4096);
        const auto t0 = clock::now();
        threaded(kThreads, kOps, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                void* p = pool.allocate();
                static_cast<Obj64*>(p)->a = i;
                pool.deallocate(p);
            }
        });
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        print_result({"synced pool 8-thread", s, kOps});
    }
    {
        fastpool::ThreadLocalPoolAllocator<sizeof(Obj64), alignof(Obj64)> pool(4096);
        const auto t0 = clock::now();
        threaded(kThreads, kOps, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                void* p = pool.allocate();
                static_cast<Obj64*>(p)->a = i;
                pool.deallocate(p);
            }
        });
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        print_result({"thread-local pool 8-thread", s, kOps});
    }
    {
        const auto t0 = clock::now();
        threaded(kThreads, kOps, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                void* p = ::operator new(sizeof(Obj64));
                static_cast<Obj64*>(p)->a = i;
                ::operator delete(p);
            }
        });
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        print_result({"new/delete 8-thread", s, kOps});
    }

    return 0;
}
