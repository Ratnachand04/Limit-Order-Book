# Limit Order Book Simulator & Market-Making Study
# Top-level build.  See docs/BUILDING.md.
#
# CLAUDE.md tech constraints encoded here:
#   * C++20, CMake >= 3.22
#   * -Wall -Wextra -Werror  (MSVC: /W4 /WX /permissive-)
#   * GoogleTest, Google Benchmark
#   * debug CI runs -fsanitize=address,undefined
cmake_minimum_required(VERSION 3.22)

project(lob_sim
        VERSION 0.1.0
        DESCRIPTION "Queue-position-aware limit order book simulator and market-making backtester"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LOB_BUILD_TESTS  "Build the GoogleTest suite"            ON)
option(LOB_BUILD_BENCH  "Build the Google Benchmark targets"    OFF)
option(LOB_BUILD_APPS   "Build the command-line runners"        ON)
option(LOB_SANITIZE     "Enable address+undefined sanitizers"   OFF)
option(LOB_WERROR       "Treat warnings as errors"              ON)
option(LOB_STRICT_WARNINGS "Add -Wconversion/-Wold-style-cast/... (GCC-Clang only)" OFF)
option(LOB_DUAL_BOOK    "Compile the dense/map dual-run assertions into the book core" OFF)

# ---------------------------------------------------------------------------
# Language / global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC: make every runtime consistent so FetchContent'd gtest links.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------
# lob_flags -- the warning/standard interface every target links against
# ---------------------------------------------------------------------------
add_library(lob_flags INTERFACE)

if(MSVC)
  target_compile_options(lob_flags INTERFACE
      /W4                 # closest MSVC analogue of -Wall -Wextra
      /permissive-        # standards conformance
      /utf-8              # source and execution charset
      /Zc:preprocessor    # conforming preprocessor
      /Zc:__cplusplus     # report the real __cplusplus value
      /EHsc
      /bigobj
      /wd4127             # conditional expression is constant (if constexpr fallbacks)
  )
  target_compile_definitions(lob_flags INTERFACE
      NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  if(LOB_WERROR)
    target_compile_options(lob_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(lob_flags INTERFACE
      -Wall -Wextra          # CLAUDE.md baseline
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wunused
  )
  # Opt-in extras.  Kept off by default because they are not part of the
  # CLAUDE.md contract and only one toolchain is verified locally; turning them
  # on with -Werror should be a deliberate, verified step, not a silent promise.
  if(LOB_STRICT_WARNINGS)
    target_compile_options(lob_flags INTERFACE
        -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wdouble-promotion)
  endif()
  if(LOB_WERROR)
    target_compile_options(lob_flags INTERFACE -Werror)
  endif()
endif()

# Sanitizers (CLAUDE.md: debug CI runs -fsanitize=address,undefined).
if(LOB_SANITIZE)
  if(MSVC)
    # MSVC ships ASan only; UBSan has no MSVC equivalent.
    target_compile_options(lob_flags INTERFACE /fsanitize=address /Zi)
    message(STATUS "LOB: MSVC AddressSanitizer enabled (UBSan unavailable on MSVC)")
  else()
    target_compile_options(lob_flags INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
    message(STATUS "LOB: AddressSanitizer + UndefinedBehaviorSanitizer enabled")
  endif()
endif()

if(LOB_DUAL_BOOK)
  target_compile_definitions(lob_flags INTERFACE LOB_DUAL_BOOK=1)
endif()

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
# Added in dependency order:  common <- {bookcore, analytics, converter} <- sim <- strategy
add_subdirectory(common)
add_subdirectory(bookcore)
add_subdirectory(analytics)
add_subdirectory(converter)
add_subdirectory(sim)
add_subdirectory(strategy)

if(LOB_BUILD_APPS)
  add_subdirectory(apps)
endif()

# ---------------------------------------------------------------------------
# Third-party (fetched only when actually needed)
# ---------------------------------------------------------------------------
if(LOB_BUILD_TESTS OR LOB_BUILD_BENCH)
  include(FetchContent)
  set(FETCHCONTENT_QUIET OFF)
endif()

if(LOB_BUILD_TESTS)
  enable_testing()

  FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2
      GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
  set(INSTALL_GTEST         OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK           ON  CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCH)
  FetchContent_Declare(benchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG        v1.9.0
      GIT_SHALLOW    TRUE)
  set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR      OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "  lob_sim ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests        : ${LOB_BUILD_TESTS}")
message(STATUS "  benchmarks   : ${LOB_BUILD_BENCH}")
message(STATUS "  sanitizers   : ${LOB_SANITIZE}")
message(STATUS "  dual book    : ${LOB_DUAL_BOOK}")
message(STATUS "----------------------------------------------------------")
