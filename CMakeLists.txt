cmake_minimum_required(VERSION 3.20)
project(fastpool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

function(fastpool_exe name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /O2 /W4)
    else()
        target_compile_options(${name} PRIVATE -O2 -Wall -Wextra -Wpedantic)
    endif()
endfunction()

fastpool_exe(tests tests/tests.cpp)
fastpool_exe(bench_threadpool bench/bench_threadpool.cpp)
fastpool_exe(bench_allocator bench/bench_allocator.cpp)

# Separate sanitizer binaries. ASan and TSan cannot be combined in one link.
# These stay off the default ALL target so a stock `cmake --build` stays fast.
if(NOT MSVC)
    add_executable(tests_asan tests/tests.cpp)
    target_include_directories(tests_asan PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(tests_asan PRIVATE Threads::Threads)
    target_compile_options(tests_asan PRIVATE -O1 -g -Wall -Wextra -fsanitize=address,undefined)
    target_link_options(tests_asan PRIVATE -fsanitize=address,undefined)
    set_target_properties(tests_asan PROPERTIES EXCLUDE_FROM_ALL TRUE)

    add_executable(tests_tsan tests/tests.cpp)
    target_include_directories(tests_tsan PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(tests_tsan PRIVATE Threads::Threads)
    target_compile_options(tests_tsan PRIVATE -O1 -g -Wall -Wextra -fsanitize=thread)
    target_link_options(tests_tsan PRIVATE -fsanitize=thread)
    set_target_properties(tests_tsan PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
