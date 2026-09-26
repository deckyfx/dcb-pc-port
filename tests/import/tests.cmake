# Asset importer (src/hle/cdrom/importer.*): synthetic disc, no game data needed.
add_executable(test_importer ${CMAKE_CURRENT_LIST_DIR}/test_importer.cpp)
target_link_libraries(test_importer PRIVATE psx_hle dcb::warnings)
add_test(NAME import.importer COMMAND test_importer)

# Byte-identical output to tools/disc/extract_disc.py on the same synthetic disc.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME import.python_parity
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_IMPORTER=$<TARGET_FILE:test_importer>
                     -DPYTHON=${Python3_EXECUTABLE}
                     -DEXTRACT_DISC=${CMAKE_SOURCE_DIR}/tools/disc/extract_disc.py
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/import_parity
                     -P ${CMAKE_CURRENT_LIST_DIR}/python_parity.cmake)
endif()
