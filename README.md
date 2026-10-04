# fastpool

Header-only C++26 thread pool and fixed-size block allocator, plus tests and
microbenchmarks. No third-party libraries.

## Design

### ThreadPool

A fixed set of worker threads pulls `MoveOnlyTask` objects from a
`std::queue` guarded by a mutex and `condition_variable`.

- `submit(f, args...)` wraps the callable in `std::packaged_task` so the
  caller gets a `std::future<T>` and exceptions propagate through `.get()`.
- The queue stores a move-only type-erased callable. `std::function` cannot
  hold `std::packaged_task` because it is not copyable.
- Shutdown (destructor) sets a stop flag, `notify_all()`, drains work that
  is already queued, then joins. Further `submit()` calls throw.
- This is intentionally not a lock-free work-stealing pool. Correctness and
  a short implementation come first; the shared mutex is the scaling limit.

### PoolAllocator

An intrusive LIFO free list of equal-sized blocks.

- `allocate()` / `deallocate()` are O(1) except when the pool grows by one
  chunk.
- Block stride is `max(BlockSize, sizeof(void*))` rounded up to the
  requested alignment. Chunks are `malloc`'d with a small prefix so the
  payload is alignment-safe without relying on sized aligned `operator new`
  (which has been unreliable on MinGW).
- `SynchronizedPoolAllocator` puts a mutex around one pool (safe, but the
  lock often loses to the CRT under contention).
- `ThreadLocalPoolAllocator` gives each thread its own pool (no lock).
  Allocate and free must happen on the same thread. The per-thread arena is
  a trivial TLS pointer; a `thread_local` object with a non-trivial
  destructor deadlocked / heap-corrupted under MinGW when many
  `std::thread` workers started at once.

## Build

Requires a C++26 compiler (GCC 15+ is known to work).

```bash
# Makefile (used when CMake is not installed)
make -j$(nproc)

# CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --build build --target tests_asan tests_tsan   # sanitizers, GCC/Clang
```

Windows (MinGW): `mingw32-make -j4` or Git Bash / MSYS `./run_all.sh`.

### One-shot

On Windows PowerShell, do **not** type `./run_all.sh` — `.sh` is opened in the editor.
Use the PowerShell script instead:

```powershell
$env:Path = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:Path
.\run_all.ps1
```

To actually execute the bash script:

```powershell
bash ./run_all.sh
```

```bash
./run_all.sh
```

Builds `-O2 -pthread`, runs tests, tries ASan+UBSan and TSan as separate
binaries (they cannot be combined), then runs both benches and prints a
summary.

This MinGW-w64 GCC 15 install does not ship `libasan` / `libtsan`, so the
sanitizer targets compile on Linux/Clang more reliably than on Windows.

## Run binaries

```bash
./build/tests
./build/bench_threadpool
./build/bench_allocator
```

## Results

Measured on Windows 11, GCC 15.2.0 (MinGW-w64), `-std=c++26 -O2 -pthread`,
32 hardware threads.

### Thread pool vs `std::async(std::launch::async)`

| Workload | Implementation | Time (s) | Tasks/s |
|---|---|---:|---:|
| 100k tiny | `ThreadPool` (32 workers) | 2.052 | 48,741 |
| 100k tiny | `std::async` | 24.875 | 4,020 |
| 1k CPU-heavy | `ThreadPool` | 0.126 | 7,907 |
| 1k CPU-heavy | `std::async` | 0.272 | 3,680 |

Tiny work is `x+1`. Heavy work is a 250k-iteration integer mix per task.

### Allocator vs `new`/`delete` (64-byte objects, 1M alloc+free pairs)

| Setup | Implementation | Time (s) | Alloc+free/s |
|---|---|---:|---:|
| serial LIFO | `PoolAllocator` | 0.0051 | 1.96e8 |
| serial LIFO | `new`/`delete` | 0.065 | 1.54e7 |
| serial burst-4096 | `PoolAllocator` | 0.0084 | 1.19e8 |
| serial burst-4096 | `new`/`delete` | 0.131 | 7.65e6 |
| 8-thread | `SynchronizedPoolAllocator` | 0.163 | 6.12e6 |
| 8-thread | `ThreadLocalPoolAllocator` | 0.0029 | 3.42e8 |
| 8-thread | `new`/`delete` | 0.012 | 8.63e7 |

## Layout

```
include/threadpool.hpp
include/pool_allocator.hpp
tests/tests.cpp
bench/bench_threadpool.cpp
bench/bench_allocator.cpp
CMakeLists.txt
Makefile
run_all.sh
```
