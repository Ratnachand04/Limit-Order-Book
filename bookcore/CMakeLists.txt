# lob_bookcore -- limit order book reconstruction (master plan §4.4).
add_library(lob_bookcore STATIC
    src/map_book.cpp
    src/dense_book.cpp
    src/dual_book.cpp
)

target_include_directories(lob_bookcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_bookcore PUBLIC lob_common)
add_library(lob::bookcore ALIAS lob_bookcore)
