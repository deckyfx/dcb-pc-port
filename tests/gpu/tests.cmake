# GPU device model (src/hle/gpu): one executable, one ctest entry per case.
add_executable(test_gpu ${CMAKE_CURRENT_LIST_DIR}/test_gpu.cpp)
target_link_libraries(test_gpu PRIVATE psx_hle dcb::warnings)
foreach(gpu_case IN ITEMS fill_rect vram_transfer triangle_fill_rule gouraud textured_sprite
                          semi_transparency draw_area_clipping polyline display)
    add_test(NAME gpu.${gpu_case} COMMAND test_gpu ${gpu_case})
endforeach()
