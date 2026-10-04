#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

namespace fastpool {

namespace detail {

constexpr std::size_t max_size(std::size_t a, std::size_t b) noexcept {
    return a > b ? a : b;
}

constexpr std::size_t round_up(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1u) & ~(align - 1u);
}

}  // namespace detail

// Fixed-size block pool with an intrusive LIFO free list.
//
// allocate() / deallocate() are O(1) aside from rare chunk growth.
// Each free block stores a next-pointer in its first bytes, so the live
// object size must be at least sizeof(void*) internally (we take the max).
//
// Not thread-safe. Use SynchronizedPoolAllocator when sharing one pool,
// or ThreadLocalPoolAllocator when each thread allocates and frees its own
// blocks (avoids the lock, but cross-thread free is a contract violation:
// chunks are owned by the allocating thread's pool).
template <std::size_t BlockSize, std::size_t Alignment = alignof(std::max_align_t)>
class PoolAllocator {
public:
    static constexpr std::size_t requested_block_size = BlockSize;
    static constexpr std::size_t alignment =
        detail::max_size(Alignment, alignof(void*));
    static constexpr std::size_t block_stride = detail::round_up(
        detail::max_size(BlockSize, sizeof(void*)), alignment);

    explicit PoolAllocator(std::size_t blocks_per_chunk = 1024)
        : blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk) {}

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    PoolAllocator(PoolAllocator&& other) noexcept
        : chunks_(std::move(other.chunks_)),
          free_list_(other.free_list_),
          blocks_per_chunk_(other.blocks_per_chunk_),
          in_use_(other.in_use_) {
        other.free_list_ = nullptr;
        other.in_use_ = 0;
    }

    PoolAllocator& operator=(PoolAllocator&& other) noexcept {
        if (this != &other) {
            release_chunks();
            chunks_ = std::move(other.chunks_);
            free_list_ = other.free_list_;
            blocks_per_chunk_ = other.blocks_per_chunk_;
            in_use_ = other.in_use_;
            other.free_list_ = nullptr;
            other.in_use_ = 0;
        }
        return *this;
    }

    ~PoolAllocator() { release_chunks(); }

    [[nodiscard]] void* allocate() {
        if (free_list_ == nullptr) {
            grow();
        }
        Node* node = free_list_;
        free_list_ = node->next;
        ++in_use_;
        return node;
    }

    void deallocate(void* p) noexcept {
        if (p == nullptr) {
            return;
        }
        auto* node = static_cast<Node*>(p);
        node->next = free_list_;
        free_list_ = node;
        --in_use_;
    }

    [[nodiscard]] std::size_t blocks_in_use() const noexcept { return in_use_; }

    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

    // How many free-list entries are currently cached (not in use).
    [[nodiscard]] std::size_t cached_free_blocks() const noexcept {
        std::size_t n = 0;
        for (Node* it = free_list_; it != nullptr; it = it->next) {
            ++n;
        }
        return n;
    }

private:
    struct Node {
        Node* next;
    };

    static void* allocate_chunk(std::size_t bytes) {
        // Over-allocate and store the original malloc pointer just before
        // the aligned address. This avoids MinGW aligned-new / _aligned_malloc
        // teardown bugs and stays O(1) at allocate/deallocate of blocks.
        const std::size_t extra = alignment + sizeof(void*);
        void* base = std::malloc(bytes + extra);
        if (base == nullptr) {
            throw std::bad_alloc();
        }
        auto addr = reinterpret_cast<std::uintptr_t>(base) + sizeof(void*);
        const auto aligned = (addr + (alignment - 1u)) & ~(alignment - 1u);
        reinterpret_cast<void**>(aligned)[-1] = base;
        return reinterpret_cast<void*>(aligned);
    }

    static void free_chunk(void* raw) noexcept {
        if (raw == nullptr) {
            return;
        }
        std::free(reinterpret_cast<void**>(raw)[-1]);
    }

    void grow() {
        const std::size_t bytes = block_stride * blocks_per_chunk_;
        void* raw = allocate_chunk(bytes);
        try {
            chunks_.push_back(raw);
        } catch (...) {
            free_chunk(raw);
            throw;
        }

        auto* base = static_cast<std::byte*>(raw);
        // Push new blocks onto the free list (LIFO). Alignment of each block
        // follows from aligned chunk base + stride multiple of alignment.
        for (std::size_t i = 0; i < blocks_per_chunk_; ++i) {
            auto* node = reinterpret_cast<Node*>(base + i * block_stride);
            node->next = free_list_;
            free_list_ = node;
        }
    }

    void release_chunks() noexcept {
        for (void* raw : chunks_) {
            free_chunk(raw);
        }
        chunks_.clear();
        free_list_ = nullptr;
        in_use_ = 0;
    }

    std::vector<void*> chunks_;
    Node* free_list_ = nullptr;
    std::size_t blocks_per_chunk_;
    std::size_t in_use_ = 0;
};

template <std::size_t BlockSize, std::size_t Alignment = alignof(std::max_align_t)>
class SynchronizedPoolAllocator {
public:
    explicit SynchronizedPoolAllocator(std::size_t blocks_per_chunk = 1024)
        : pool_(blocks_per_chunk) {}

    [[nodiscard]] void* allocate() {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.allocate();
    }

    void deallocate(void* p) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        pool_.deallocate(p);
    }

    [[nodiscard]] std::size_t blocks_in_use() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.blocks_in_use();
    }

private:
    PoolAllocator<BlockSize, Alignment> pool_;
    mutable std::mutex mu_;
};

// One PoolAllocator per thread, per (BlockSize, Alignment) pair.
// Fast path has no atomics and no mutex. allocate/deallocate must be paired
// on the same thread; otherwise a block from thread A's chunk can land on
// thread B's free list while A later frees the chunk on thread exit.
template <std::size_t BlockSize, std::size_t Alignment = alignof(std::max_align_t)>
class ThreadLocalPoolAllocator {
public:
    explicit ThreadLocalPoolAllocator(std::size_t blocks_per_chunk = 1024)
        : blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk) {}

    [[nodiscard]] void* allocate() { return local().allocate(); }

    void deallocate(void* p) noexcept { local().deallocate(p); }

    [[nodiscard]] std::size_t blocks_in_use() const { return local().blocks_in_use(); }

private:
    static PoolAllocator<BlockSize, Alignment>& tls_pool(std::size_t blocks_per_chunk) {
        // Use a trivial TLS pointer. A thread_local PoolAllocator (non-trivial
        // ctor/dtor) in an inline template has deadlocked and heap-corrupted
        // under MinGW/libstdc++ when many std::thread workers start at once.
        // The arena is released at process exit; pair alloc/free on one thread.
        thread_local PoolAllocator<BlockSize, Alignment>* pool = nullptr;
        if (pool == nullptr) {
            pool = new PoolAllocator<BlockSize, Alignment>(blocks_per_chunk);
        }
        return *pool;
    }

    PoolAllocator<BlockSize, Alignment>& local() const {
        return tls_pool(blocks_per_chunk_);
    }

    std::size_t blocks_per_chunk_;
};

}  // namespace fastpool
