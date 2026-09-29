add_library(decoder_jpeg STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/Jpeg.cpp)
target_include_directories(decoder_jpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(decoder_jpeg SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/stb)
set_target_properties(decoder_jpeg PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
)

if(BUILD_TESTING)
    add_executable(decoder_jpeg_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/Jpeg.cpp)
    target_link_libraries(decoder_jpeg_tests PRIVATE decoder_jpeg)
    add_test(NAME decoder_jpeg COMMAND decoder_jpeg_tests)
endif()
