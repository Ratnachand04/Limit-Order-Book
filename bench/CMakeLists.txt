# Google Benchmark targets (master plan §4.10).
#
# Targets to beat, all recorded in bench/RESULTS.md with the exact hardware:
#   converter                >= 1M msg/s
#   replay (book + strategy) >= 300-500k events/s, single-threaded
#   DenseBook::ApplyDepth    p50 < 100 ns, p99 < 1 us
#
# The p99 matters more than the mean: a book update that is usually 60 ns and
# occasionally 40 us is a book update that stalls the replay loop (§4.10).
add_executable(lob_bench
    bench_book.cpp
    bench_converter.cpp
    bench_replay.cpp
    ${CMAKE_SOURCE_DIR}/tests/test_support.cpp
)

target_include_directories(lob_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lob_bench PRIVATE lob_strategy benchmark::benchmark)
target_compile_definitions(lob_bench PRIVATE LOB_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
