# lob_common -- types, parsers, config and I/O primitives shared by every module.
add_library(lob_common STATIC
    src/instrument.cpp
    src/execution.cpp
    src/json.cpp
    src/yaml.cpp
    src/config.cpp
    src/csv_writer.cpp
)

target_include_directories(lob_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_common PUBLIC lob_flags)
add_library(lob::common ALIAS lob_common)
