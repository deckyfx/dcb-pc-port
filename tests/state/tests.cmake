# Save states: the serialisation layer (src/runtime/src/state.cpp).
add_executable(test_state_stream ${CMAKE_CURRENT_LIST_DIR}/test_state_stream.cpp)
target_link_libraries(test_state_stream PRIVATE psx_runtime dcb::warnings)
add_test(NAME state.stream COMMAND test_state_stream)
