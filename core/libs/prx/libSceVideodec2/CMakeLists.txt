add_library(libSceVideodec2 SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoDecoder.cpp
)

target_include_directories(libSceVideodec2 PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# H.264 and HEVC are decoded with the system FFmpeg, as in shadPS4. Without it the library still
# builds, and creating a decoder reports the missing codec.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG_VIDEO IMPORTED_TARGET libavcodec libavutil libswscale)
endif()
if(FFMPEG_VIDEO_FOUND)
    target_link_libraries(libSceVideodec2 PRIVATE PkgConfig::FFMPEG_VIDEO)
    target_compile_definitions(libSceVideodec2 PRIVATE APS5_HAVE_FFMPEG=1)
else()
    message(WARNING "FFmpeg (libavcodec, libavutil, libswscale) not found: libSceVideodec2 cannot decode video")
endif()

target_link_libraries(libSceVideodec2 PRIVATE libc libkernel)
configure_windows_unwind(libSceVideodec2)
