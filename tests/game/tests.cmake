# Game-override logic that needs no guest memory (header-only helpers of src/game/overrides).

add_executable(test_game_mini_fit ${CMAKE_CURRENT_LIST_DIR}/test_mini_fit.cpp)
target_include_directories(test_game_mini_fit PRIVATE ${CMAKE_SOURCE_DIR}/src/game/overrides)
target_link_libraries(test_game_mini_fit PRIVATE dcb::warnings)
add_test(NAME game.mini_fit COMMAND test_game_mini_fit)
