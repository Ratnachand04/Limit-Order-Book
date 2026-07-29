# lob_sim -- event loop, virtual clock, latency, order gateway, queue tracker,
# probe engine and the replay engine itself (master plan §§4.5-4.6, §3.7, §3.8).
add_library(lob_sim STATIC
    src/gateway.cpp
    src/queue_tracker.cpp
    src/probe.cpp
    src/simulator.cpp
)

target_include_directories(lob_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_sim PUBLIC lob_common lob_bookcore lob_analytics lob_converter)
add_library(lob::sim ALIAS lob_sim)
