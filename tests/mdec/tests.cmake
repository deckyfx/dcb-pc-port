# MDEC device model (src/hle/mdec).
add_executable(test_mdec ${CMAKE_CURRENT_LIST_DIR}/test_mdec.cpp)
target_link_libraries(test_mdec PRIVATE psx_hle dcb::warnings)
add_test(NAME mdec.decoder COMMAND test_mdec)
