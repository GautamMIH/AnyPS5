add_library(libSceAudiodec.native SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AacDecoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Codecs.cpp
)

target_include_directories(libSceAudiodec.native PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/3rdparty/LibAtrac9/C/src
)

# AAC and MP3 decode with FFmpeg: on Linux the system's (as shadPS4 does for AAC, MP3 and video),
# elsewhere the bundled ffmpeg-core. ATRAC9 decodes with LibAtrac9 (the atrac9 target of
# libSceAjm.native).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG_AUDIO REQUIRED IMPORTED_TARGET libavcodec libavutil)
    target_link_libraries(libSceAudiodec.native PRIVATE PkgConfig::FFMPEG_AUDIO)
else()
    target_link_libraries(libSceAudiodec.native PRIVATE ffmpeg)
endif()
target_compile_definitions(libSceAudiodec.native PRIVATE APS5_HAVE_FFMPEG=1)

target_link_libraries(libSceAudiodec.native PRIVATE atrac9 libc libkernel)
configure_windows_unwind(libSceAudiodec.native)

if(MINGW)
    foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
        string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    endforeach()
endif()
