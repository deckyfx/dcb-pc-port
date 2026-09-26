# Save states: the serialisation layer (src/runtime/src/state.cpp) and device round trips.
add_executable(test_state_stream ${CMAKE_CURRENT_LIST_DIR}/test_state_stream.cpp)
target_link_libraries(test_state_stream PRIVATE psx_runtime dcb::warnings)
add_test(NAME state.stream COMMAND test_state_stream)

# Bios and Mmio reach the dispatcher: link the empty function table in place of generated code.
add_executable(test_state_devices ${CMAKE_CURRENT_LIST_DIR}/test_state_devices.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/stub/empty_function_table.c)
target_link_libraries(test_state_devices PRIVATE psx_hle dcb::warnings)
add_test(NAME state.devices COMMAND test_state_devices)
