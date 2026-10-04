# English-data builder (src/patch): the text pipeline's pure pieces on synthetic data.
add_executable(test_patch_text ${CMAKE_CURRENT_LIST_DIR}/test_patch_text.cpp)
target_link_libraries(test_patch_text PRIVATE dcb_patch dcb::warnings)
add_test(NAME patch.text COMMAND test_patch_text)

# Gameplay mods (src/patch/mods.cpp): the boss rematch on a synthetic city script.
add_executable(test_patch_mods ${CMAKE_CURRENT_LIST_DIR}/test_patch_mods.cpp)
target_link_libraries(test_patch_mods PRIVATE dcb_patch dcb::warnings)
add_test(NAME patch.mods COMMAND test_patch_mods)
