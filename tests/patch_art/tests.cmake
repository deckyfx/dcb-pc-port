# The US art swap (src/patch/art_swap.hpp): helper cases. The whole swap is compared
# with the Python pipeline on real dumps by tools/patch/compare_art.sh.
add_executable(test_patch_art ${CMAKE_CURRENT_LIST_DIR}/test_art.cpp)
target_link_libraries(test_patch_art PRIVATE dcb_patch dcb::warnings)
foreach(art_case IN ITEMS prewarp narrow reshape read_tim compose fit)
    add_test(NAME patch.art_${art_case} COMMAND test_patch_art ${art_case})
endforeach()
