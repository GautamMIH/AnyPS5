add_library(libSceAudiodec.native SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AacDecoder.cpp
)

target_include_directories(libSceAudiodec.native PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# AAC is decoded with the system FFmpeg (as shadPS4 does for AAC, MP3 and video). Without it the
# library still builds, and creating an AAC decoder reports the missing codec.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG_AUDIO IMPORTED_TARGET libavcodec libavutil)
endif()
if(FFMPEG_AUDIO_FOUND)
    target_link_libraries(libSceAudiodec.native PRIVATE PkgConfig::FFMPEG_AUDIO)
    target_compile_definitions(libSceAudiodec.native PRIVATE APS5_HAVE_FFMPEG=1)
else()
    message(WARNING "FFmpeg (libavcodec, libavutil) not found: libSceAudiodec cannot decode AAC")
endif()

target_link_libraries(libSceAudiodec.native PRIVATE libc libkernel)
configure_windows_unwind(libSceAudiodec.native)
