add_library(libSceCesCs-module SHARED EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/../libSceCesCs/Export.cpp)
target_include_directories(libSceCesCs-module PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceCesCs-module)
