# BIOS memory-card file system (src/hle/mcrd).
add_executable(test_card_fs ${CMAKE_CURRENT_LIST_DIR}/test_card_fs.cpp)
target_link_libraries(test_card_fs PRIVATE psx_hle dcb::warnings)
add_test(NAME mcrd.card_fs COMMAND test_card_fs)
