# CD-ROM controller model (src/hle/cdrom).
add_executable(test_cdrom ${CMAKE_CURRENT_LIST_DIR}/test_cdrom.cpp)
target_link_libraries(test_cdrom PRIVATE psx_hle dcb::warnings)
add_test(NAME cdrom.controller COMMAND test_cdrom)

# Load-log sector->file mapping (src/hle/cdrom/load_log.*).
add_executable(test_load_log ${CMAKE_CURRENT_LIST_DIR}/test_load_log.cpp)
target_link_libraries(test_load_log PRIVATE psx_hle dcb::warnings)
add_test(NAME cdrom.load_log COMMAND test_load_log)
