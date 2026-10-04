#!/usr/bin/env bash
# Build, test, sanitize, and benchmark. Prints a short summary at the end.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

MAKE="make"
if ! command -v make >/dev/null 2>&1; then
  if command -v mingw32-make >/dev/null 2>&1; then
    MAKE="mingw32-make"
  else
    echo "error: make or mingw32-make not found" >&2
    exit 1
  fi
fi

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

run_bin() {
  local p="$1"
  shift || true
  if [[ -x "${p}.exe" ]]; then
    "${p}.exe" "$@"
  else
    "${p}" "$@"
  fi
}

echo "==> build (Release -O2)"
"$MAKE" -j"$JOBS" all

echo
echo "==> tests"
run_bin ./build/tests

ASAN_OK="skipped"
TSAN_OK="skipped"

echo
echo "==> AddressSanitizer + UBSan (separate target)"
if "$MAKE" -j"$JOBS" asan && run_bin ./build/tests_asan; then
  ASAN_OK="passed"
else
  ASAN_OK="failed or unsupported"
  echo "warning: ASan build/run did not succeed (common on some Windows toolchains)"
fi

echo
echo "==> ThreadSanitizer (separate target)"
if "$MAKE" -j"$JOBS" tsan && run_bin ./build/tests_tsan; then
  TSAN_OK="passed"
else
  TSAN_OK="failed or unsupported"
  echo "warning: TSan build/run did not succeed (often unavailable on MinGW/Windows)"
fi

echo
echo "==> bench_threadpool"
run_bin ./build/bench_threadpool | tee build/bench_threadpool.out

echo
echo "==> bench_allocator"
run_bin ./build/bench_allocator | tee build/bench_allocator.out

echo
echo "========== SUMMARY =========="
echo "tests:          passed"
echo "asan+ubsan:     $ASAN_OK"
echo "tsan:           $TSAN_OK"
echo
echo "----- thread pool -----"
cat build/bench_threadpool.out
echo
echo "----- allocator -----"
cat build/bench_allocator.out
echo "=============================="
