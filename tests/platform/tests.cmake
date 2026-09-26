# Platform-layer tests: pure conversion logic and the headless backend (no display server needed).

add_executable(test_platform_display ${CMAKE_CURRENT_LIST_DIR}/test_display.cpp)
target_link_libraries(test_platform_display PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.display COMMAND test_platform_display)

add_executable(test_platform_headless ${CMAKE_CURRENT_LIST_DIR}/test_headless.cpp)
target_link_libraries(test_platform_headless PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.headless COMMAND test_platform_headless)
