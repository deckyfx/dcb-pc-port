# Trainer tests: cheat-code parsing and engine, memory search, the partner editor and the panel
# logic, all on plain RAM buffers (no SDL, no game).

add_executable(test_trainer_cheats ${CMAKE_CURRENT_LIST_DIR}/test_cheats.cpp)
target_link_libraries(test_trainer_cheats PRIVATE dcb_platform dcb::warnings)
target_compile_definitions(test_trainer_cheats PRIVATE DCB_CHEATS_EXAMPLE="${CMAKE_SOURCE_DIR}/docs/cheats.example.txt")
add_test(NAME trainer.cheats COMMAND test_trainer_cheats)

add_executable(test_trainer_search ${CMAKE_CURRENT_LIST_DIR}/test_search.cpp)
target_link_libraries(test_trainer_search PRIVATE dcb_platform dcb::warnings)
add_test(NAME trainer.search COMMAND test_trainer_search)

add_executable(test_trainer_panel ${CMAKE_CURRENT_LIST_DIR}/test_panel.cpp)
target_link_libraries(test_trainer_panel PRIVATE dcb_platform dcb::warnings)
add_test(NAME trainer.panel COMMAND test_trainer_panel)

add_executable(test_trainer_partners ${CMAKE_CURRENT_LIST_DIR}/test_partners.cpp)
target_link_libraries(test_trainer_partners PRIVATE dcb_platform dcb::warnings)
add_test(NAME trainer.partners COMMAND test_trainer_partners)
