# Command-line runners.  Every result in the paper comes from one of these.
add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob_strategy)

add_executable(lob_sweep lob_sweep.cpp)
target_link_libraries(lob_sweep PRIVATE lob_strategy)

add_executable(lob_calibrate lob_calibrate.cpp)
target_link_libraries(lob_calibrate PRIVATE lob_bookcore lob_converter)
