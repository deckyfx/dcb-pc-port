# Controller port model (src/hle/pad).
add_executable(test_sio ${CMAKE_CURRENT_LIST_DIR}/test_sio.cpp)
target_link_libraries(test_sio PRIVATE psx_hle dcb::warnings)
add_test(NAME pad.sio COMMAND test_sio)
