# GTE (COP2) register file and commands (src/runtime/src/gte.cpp).
add_executable(test_gte ${CMAKE_CURRENT_LIST_DIR}/test_gte.cpp)
target_link_libraries(test_gte PRIVATE psx_runtime dcb::warnings)
add_test(NAME gte.commands COMMAND test_gte)
