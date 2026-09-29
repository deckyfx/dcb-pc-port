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

add_executable(test_platform_input_log ${CMAKE_CURRENT_LIST_DIR}/test_input_log.cpp)
target_link_libraries(test_platform_input_log PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.input_log COMMAND test_platform_input_log)

add_executable(test_platform_menu ${CMAKE_CURRENT_LIST_DIR}/test_menu.cpp)
target_link_libraries(test_platform_menu PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.menu COMMAND test_platform_menu)

add_executable(test_platform_memcard ${CMAKE_CURRENT_LIST_DIR}/test_memcard.cpp)
target_link_libraries(test_platform_memcard PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.memcard COMMAND test_platform_memcard)

add_executable(test_platform_text_catalog ${CMAKE_CURRENT_LIST_DIR}/test_text_catalog.cpp)
target_link_libraries(test_platform_text_catalog PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.text_catalog COMMAND test_platform_text_catalog)

add_executable(test_platform_text_codes ${CMAKE_CURRENT_LIST_DIR}/test_text_codes.cpp)
target_link_libraries(test_platform_text_codes PRIVATE dcb_platform dcb::warnings)
add_test(NAME platform.text_codes COMMAND test_platform_text_codes)
