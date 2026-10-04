#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastpool {

// Move-only type-erased callable. std::function requires CopyConstructible
// targets, but std::packaged_task is move-only, so the queue cannot use it.
class MoveOnlyTask {
public:
    MoveOnlyTask() = default;

    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, MoveOnlyTask>)
    explicit MoveOnlyTask(F&& f)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;

    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    void operator()() {
        impl_->invoke();
        impl_.reset();
    }

    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke() = 0;
    };

    template <class F>
    struct Model final : Concept {
        explicit Model(F&& f) : fn_(std::move(f)) {}
        void invoke() override { fn_(); }
        F fn_;
    };

    std::unique_ptr<Concept> impl_;
};

// Fixed-size worker pool. Tasks are queued under a mutex and workers sleep
// on a condition_variable. This is not a lock-free work-stealing pool: the
// goal is a correct, small, teachable implementation with a clean shutdown
// path. The shared queue becomes the bottleneck under very high submit rates.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t nthreads = 0) {
        if (nthreads == 0) {
            nthreads = std::thread::hardware_concurrency();
        }
        if (nthreads == 0) {
            nthreads = 1;
        }

        workers_.reserve(nthreads);
        try {
            for (std::size_t i = 0; i < nthreads; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            // Partial construction: stop already-started workers, then rethrow
            // so we never join a joinable thread from a destructor unwind of
            // a half-built object without signalling stop.
            request_stop_and_join();
            throw;
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Drain already-queued work, then join. New submit() calls throw.
    ~ThreadPool() { request_stop_and_join(); }

    template <class F, class... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, Args...>> {
        using R = std::invoke_result_t<std::decay_t<F>, Args...>;

        // packaged_task stores the return value / exception in the shared
        // state that the future waits on. The lambda binds arguments by
        // move so submit(f, x) does not require f to be CopyConstructible.
        std::packaged_task<R()> packaged(
            [fn = std::forward<F>(f),
             ...captured = std::forward<Args>(args)]() mutable -> R {
                return std::invoke(std::move(fn), std::move(captured)...);
            });

        std::future<R> result = packaged.get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool::submit on stopped pool");
            }
            tasks_.emplace(MoveOnlyTask([p = std::move(packaged)]() mutable { p(); }));
        }
        cv_.notify_one();
        return result;
    }

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        for (;;) {
            MoveOnlyTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // Run outside the lock so other workers can dequeue in parallel.
            task();
        }
    }

    void request_stop_and_join() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<MoveOnlyTask> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace fastpool
