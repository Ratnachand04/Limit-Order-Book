# lob_converter -- Binance JSONL decoding, book-sync protocol, binary event I/O.
add_library(lob_converter STATIC
    src/binance.cpp
    src/converter.cpp
    src/event_io.cpp
)

target_include_directories(lob_converter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_converter PUBLIC lob_common)
add_library(lob::converter ALIAS lob_converter)

if(LOB_BUILD_APPS)
  add_executable(lob_convert src/main.cpp)
  target_link_libraries(lob_convert PRIVATE lob_converter)
endif()
