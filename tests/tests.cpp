#include "pool_allocator.hpp"
#include "threadpool.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "FAIL " << file << ":" << line << "  " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

void test_threadpool_results() {
    fastpool::ThreadPool pool(4);
    constexpr int n = 1000;
    std::vector<std::future<int>> futs;
    futs.reserve(n);
    for (int i = 0; i < n; ++i) {
        futs.push_back(pool.submit([i] { return i * i; }));
    }
    long long sum = 0;
    for (auto& f : futs) {
        sum += f.get();
    }
    const long long expected = [&] {
        long long s = 0;
        for (int i = 0; i < n; ++i) {
            s += static_cast<long long>(i) * i;
        }
        return s;
    }();
    CHECK(sum == expected);
}

void test_threadpool_void_and_args() {
    fastpool::ThreadPool pool(2);
    int x = 0;
    auto f = pool.submit([](int& v) { v = 42; }, std::ref(x));
    f.get();
    CHECK(x == 42);

    auto g = pool.submit([] { return std::string("ok"); });
    CHECK(g.get() == "ok");
}

void test_threadpool_exception() {
    fastpool::ThreadPool pool(2);
    auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    bool threw = false;
    try {
        (void)f.get();
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "boom";
    }
    CHECK(threw);

    auto g = pool.submit([] { return 7; });
    CHECK(g.get() == 7);
}

void test_threadpool_shutdown_no_deadlock() {
    // Destructor must wake idle workers and join even if the queue is empty,
    // and must finish queued work if tasks are still pending.
    {
        fastpool::ThreadPool pool(8);
        std::vector<std::future<int>> futs;
        for (int i = 0; i < 200; ++i) {
            futs.push_back(pool.submit([] {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                return 1;
            }));
        }
        int sum = 0;
        for (auto& f : futs) {
            sum += f.get();
        }
        CHECK(sum == 200);
    }  // join here

    {
        fastpool::ThreadPool pool(4);
        // Leave tasks in the queue; destructor should drain them.
        for (int i = 0; i < 64; ++i) {
            (void)pool.submit([] {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            });
        }
    }
}

void test_threadpool_submit_after_stop() {
    auto* pool = new fastpool::ThreadPool(1);
    auto f = pool->submit([] { return 1; });
    CHECK(f.get() == 1);
    delete pool;
}

void test_allocator_basic_and_reuse() {
    fastpool::PoolAllocator<64> pool(8);
    void* a = pool.allocate();
    CHECK(a != nullptr);
    CHECK(pool.blocks_in_use() == 1);

    // Alignment: pointer must be aligned to the pool's alignment.
    CHECK((reinterpret_cast<std::uintptr_t>(a) % fastpool::PoolAllocator<64>::alignment) == 0);

    auto* bytes = static_cast<unsigned char*>(a);
    std::fill(bytes, bytes + 64, 0xAB);

    pool.deallocate(a);
    CHECK(pool.blocks_in_use() == 0);

    void* b = pool.allocate();
    // LIFO free list: the just-freed block is reused.
    CHECK(a == b);
    CHECK(pool.blocks_in_use() == 1);
    pool.deallocate(b);
}

void test_allocator_many_and_growth() {
    fastpool::PoolAllocator<64> pool(16);
    std::vector<void*> ptrs;
    ptrs.reserve(100);
    for (int i = 0; i < 100; ++i) {
        void* p = pool.allocate();
        CHECK(p != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(p) % 8) == 0);
        static_cast<int*>(p)[0] = i;
        ptrs.push_back(p);
    }
    CHECK(pool.blocks_in_use() == 100);
    CHECK(pool.chunk_count() >= 100 / 16);

    for (int i = 0; i < 100; ++i) {
        CHECK(static_cast<int*>(ptrs[static_cast<std::size_t>(i)])[0] == i);
    }

    for (void* p : ptrs) {
        pool.deallocate(p);
    }
    CHECK(pool.blocks_in_use() == 0);

    // After a full free, the next 100 allocations should not need new chunks
    // (free list holds them).
    const std::size_t chunks = pool.chunk_count();
    std::vector<void*> again;
    for (int i = 0; i < 100; ++i) {
        again.push_back(pool.allocate());
    }
    CHECK(pool.chunk_count() == chunks);
    for (void* p : again) {
        pool.deallocate(p);
    }
}

void test_allocator_null_free() {
    fastpool::PoolAllocator<32> pool;
    pool.deallocate(nullptr);
    CHECK(pool.blocks_in_use() == 0);
}

void test_synchronized_and_tls() {
    fastpool::SynchronizedPoolAllocator<64> sync(32);
    std::vector<std::thread> ts;
    std::vector<void*> grabbed(8, nullptr);
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&sync, &grabbed, i] {
            grabbed[static_cast<std::size_t>(i)] = sync.allocate();
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    CHECK(sync.blocks_in_use() == 8);
    for (void* p : grabbed) {
        CHECK(p != nullptr);
        sync.deallocate(p);
    }
    CHECK(sync.blocks_in_use() == 0);

    fastpool::ThreadLocalPoolAllocator<64> tls(16);
    void* p = tls.allocate();
    CHECK(p != nullptr);
    tls.deallocate(p);
    void* q = tls.allocate();
    CHECK(p == q);
    tls.deallocate(q);

    std::vector<std::thread> workers;
    std::atomic<int> done{0};
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&tls, &done] {
            void* p = tls.allocate();
            CHECK(p != nullptr);
            tls.deallocate(p);
            for (int i = 0; i < 10000; ++i) {
                void* q = tls.allocate();
                static_cast<int*>(q)[0] = i;
                tls.deallocate(q);
            }
            done.fetch_add(1);
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    CHECK(done.load() == 8);
}

}  // namespace

int main() {
    test_threadpool_results();
    test_threadpool_void_and_args();
    test_threadpool_exception();
    test_threadpool_shutdown_no_deadlock();
    test_threadpool_submit_after_stop();
    test_allocator_basic_and_reuse();
    test_allocator_many_and_growth();
    test_allocator_null_free();
    test_synchronized_and_tls();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
