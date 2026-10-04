# Fallback build when CMake is not installed.
CXX      ?= g++
CXXFLAGS ?= -std=c++26 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  ?= -pthread

BUILD := build

.PHONY: all tests benches asan tsan sanitizers clean

all: tests benches

tests: $(BUILD)/tests

benches: $(BUILD)/bench_threadpool $(BUILD)/bench_allocator

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: tests/tests.cpp include/threadpool.hpp include/pool_allocator.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/tests.cpp $(LDFLAGS)

$(BUILD)/bench_threadpool: bench/bench_threadpool.cpp include/threadpool.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ bench/bench_threadpool.cpp $(LDFLAGS)

$(BUILD)/bench_allocator: bench/bench_allocator.cpp include/pool_allocator.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ bench/bench_allocator.cpp $(LDFLAGS)

$(BUILD)/tests_asan: tests/tests.cpp include/threadpool.hpp include/pool_allocator.hpp | $(BUILD)
	$(CXX) -std=c++26 -O1 -g -Wall -Wextra -Iinclude -fsanitize=address,undefined \
		-o $@ tests/tests.cpp $(LDFLAGS)

$(BUILD)/tests_tsan: tests/tests.cpp include/threadpool.hpp include/pool_allocator.hpp | $(BUILD)
	$(CXX) -std=c++26 -O1 -g -Wall -Wextra -Iinclude -fsanitize=thread \
		-o $@ tests/tests.cpp $(LDFLAGS)

asan: $(BUILD)/tests_asan
tsan: $(BUILD)/tests_tsan
sanitizers: asan tsan

clean:
	rm -rf $(BUILD)
