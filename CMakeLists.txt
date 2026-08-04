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
)

target_link_libraries(aegis
    PRIVATE
        Boost::system
        Boost::thread
        Threads::Threads
)

# ── Tests ─────────────────────────────────────────────────────────────────────
# Kept separate so we can build/run them without starting the server.
# We'll add test targets in later stages as components are introduced.
enable_testing()
# add_subdirectory(test)   # uncomment when test sources exist
