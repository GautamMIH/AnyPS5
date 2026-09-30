add_library(decoder_png STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/Png.cpp)
target_include_directories(decoder_png PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(decoder_png SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/stb)
set_target_properties(decoder_png PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
)

if(BUILD_TESTING)
    add_executable(decoder_png_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/Png.cpp)
    target_link_libraries(decoder_png_tests PRIVATE decoder_png)
    add_test(NAME decoder_png COMMAND decoder_png_tests)
endif()
