# lob_strategy -- the S0..S3 ladder (master plan Part 6) plus the A-S / GLFT
# closed forms (§3.4, §3.5) and the sigma / flow estimators (§3.2, §3.6).
add_library(lob_strategy STATIC
    src/volatility.cpp
    src/as_math.cpp
    src/quoting_strategy.cpp
    src/strategies.cpp
    src/factory.cpp
)

target_include_directories(lob_strategy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_strategy PUBLIC lob_sim)
add_library(lob::strategy ALIAS lob_strategy)
