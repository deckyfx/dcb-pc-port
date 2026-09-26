# Platform-layer tests: pure conversion logic, the headless backend and the PC options
# (settings.ini) — no display server or audio device needed.

add_executable(test_platform_display ${CMAKE_CURRENT_LIST_DIR}/test_display.cpp)
target_link_libraries(test_platform_display PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.display COMMAND test_platform_display)

add_executable(test_platform_headless ${CMAKE_CURRENT_LIST_DIR}/test_headless.cpp)
target_link_libraries(test_platform_headless PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.headless COMMAND test_platform_headless)

add_executable(test_platform_settings ${CMAKE_CURRENT_LIST_DIR}/test_settings.cpp)
target_link_libraries(test_platform_settings PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.settings COMMAND test_platform_settings)
