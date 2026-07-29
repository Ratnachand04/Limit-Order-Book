# lob_analytics -- ledger, PnL decomposition, markouts and CSV writers (§3.9, §4.8).
add_library(lob_analytics STATIC
    src/ledger.cpp
    src/markout.cpp
    src/recorders.cpp
)

target_include_directories(lob_analytics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Depends on `common` only.  Keeping the engine out of the analytics layer is
# what lets the dependency graph stay a straight line:
#   common <- analytics <- sim <- strategy
target_link_libraries(lob_analytics PUBLIC lob_common)
add_library(lob::analytics ALIAS lob_analytics)
