add_library(libSceJpegEnc SHARED EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp)
target_include_directories(libSceJpegEnc PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceJpegEnc PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceJpegEnc)
