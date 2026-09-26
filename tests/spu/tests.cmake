# SPU and XA-ADPCM (src/hle/spu).
add_executable(test_spu ${CMAKE_CURRENT_LIST_DIR}/test_spu.cpp)
target_link_libraries(test_spu PRIVATE psx_hle dcb::warnings)
add_test(NAME spu.voices COMMAND test_spu)

add_executable(test_xa ${CMAKE_CURRENT_LIST_DIR}/test_xa.cpp)
target_link_libraries(test_xa PRIVATE psx_hle dcb::warnings)
add_test(NAME spu.xa_adpcm COMMAND test_xa)
