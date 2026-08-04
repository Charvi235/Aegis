cmake_minimum_required(VERSION 3.16)
project(aegis VERSION 0.1.0 LANGUAGES CXX)

# ── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # No GNU extensions; keep it portable

# ── Build type default ───────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ── Compiler warnings ────────────────────────────────────────────────────────
# Treat warnings as errors during development so bad patterns don't accumulate.
add_compile_options(
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# ── Dependencies ─────────────────────────────────────────────────────────────
# Boost.Asio and Boost.Beast are header-only in modern Boost, but we still
# need to find the Boost installation for includes and link system/thread.
# Beast is also header-only; it is bundled with Asio in modern Boost (>=1.66).
# No extra component is needed for Beast — just the Boost include path.
find_package(Boost 1.74 REQUIRED COMPONENTS system thread)
find_package(Threads REQUIRED)          # pthreads on Linux, native on Windows

# On Windows with MSVC, Beast uses some deprecated Winsock APIs that trigger
# warnings.  Suppress them so our -WX (warnings-as-errors) flag doesn't fire
# on code we don't own.
if(MSVC)
    add_compile_definitions(_WIN32_WINNT=0x0601)   # target Windows 7+
    add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
endif()

# ── Shared include path ──────────────────────────────────────────────────────
# Everything under include/ is available as <aegis/...> throughout the project.
include_directories(include)

# ── Main executable ──────────────────────────────────────────────────────────
add_executable(aegis
    src/main.cpp
    src/server.cpp
    src/session.cpp
    src/token_bucket.cpp
    src/rate_limiter.cpp
    src/proxy.cpp
)

target_link_libraries(aegis
    PRIVATE
        Boost::system
        Boost::thread
        Threads::Threads
)

# ── Tests ─────────────────────────────────────────────────────────────────────
# Each test is its own executable so we can run components in isolation.
# No external framework — plain C++ main() with EXPECT macros.
enable_testing()

# ── test_token_bucket ────────────────────────────────────────────────────────
# Only needs token_bucket.cpp — no Boost, no server, no HTTP.
# This is intentional: unit tests should compile fast and have no I/O side
# effects, so they can run in CI or offline without any running services.
add_executable(test_token_bucket
    test/test_token_bucket.cpp
    src/token_bucket.cpp
)
target_include_directories(test_token_bucket PRIVATE include)
# Inherit the same language-level flags as the main binary (C++17, warnings).
target_compile_features(test_token_bucket PRIVATE cxx_std_17)

add_test(NAME token_bucket COMMAND test_token_bucket)

# ── test_lru_cache ────────────────────────────────────────────────────────────
# LruCache is header-only (template), so no .cpp to compile — just the test.
add_executable(test_lru_cache
    test/test_lru_cache.cpp
)
target_include_directories(test_lru_cache PRIVATE include)
target_compile_features(test_lru_cache PRIVATE cxx_std_17)
# LruCache uses std::thread in its concurrency test, so link Threads.
target_link_libraries(test_lru_cache PRIVATE Threads::Threads)

add_test(NAME lru_cache COMMAND test_lru_cache)
