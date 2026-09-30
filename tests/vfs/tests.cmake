# Asset pipeline runtime (src/vfs) + HD texture index (src/hle/gpu): one
# executable, one ctest entry per case.
add_executable(test_vfs ${CMAKE_CURRENT_LIST_DIR}/test_vfs.cpp)
target_link_libraries(test_vfs PRIVATE psx_hle dcb_vfs dcb::warnings)
foreach(vfs_case IN ITEMS tim4 tim_stp toc_shapes tim_scan fnv pak_roundtrip payload vfs_mounts hd_replace
                          hd_identity_stp hd_identity_e2e hd_slot hd_raw hd_duplicate_colours brr_block vab_parse vab_decode)
    add_test(NAME vfs.${vfs_case} COMMAND test_vfs ${vfs_case})
endforeach()
