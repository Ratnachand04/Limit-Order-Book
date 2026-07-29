# The test suite (master plan §4.9).
#
# CLAUDE.md rule 3: "A module without tests does not exist."  Every module below
# has one, and the phase gates are encoded as named tests:
#   Phase 1  test_converter        round trip + Binance sync protocol
#   Phase 2  test_*_book, test_book_property, test_golden_replay
#   Phase 3  test_event_loop, test_determinism
#   Phase 4  test_queue_tracker, test_worked_trace
#   Phase 5  test_ledger, test_analytics
#   Phase 6  test_as_math, test_strategies
include(GoogleTest)

# Deliberately free of any GoogleTest dependency: the benchmarks reuse the same
# synthetic-data generators, and they must build with LOB_BUILD_TESTS=OFF.
add_library(lob_test_support STATIC test_support.cpp)
target_include_directories(lob_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lob_test_support PUBLIC lob_strategy)
target_compile_definitions(lob_test_support
    PUBLIC LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

set(LOB_TEST_SOURCES
    test_types.cpp
    test_instrument.cpp
    test_json.cpp
    test_config.cpp
    test_converter.cpp
    test_map_book.cpp
    test_dense_book.cpp
    test_book_property.cpp
    test_event_loop.cpp
    test_queue_tracker.cpp
    test_worked_trace.cpp
    test_ledger.cpp
    test_analytics.cpp
    test_as_math.cpp
    test_strategies.cpp
    test_determinism.cpp
    test_golden_replay.cpp
)

add_executable(lob_tests ${LOB_TEST_SOURCES})
target_link_libraries(lob_tests PRIVATE lob_test_support GTest::gtest_main)

# One CTest entry per GoogleTest case, so a failure names the exact scenario.
gtest_discover_tests(lob_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300
)
