add_library(libSceAmpr SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceAmpr PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libSceAmpr PRIVATE libkernel)
configure_windows_unwind(libSceAmpr)

if(MINGW)
    foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
        string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    endforeach()
endif()

# Upstream's tests/GuestAmpr.cpp targets its private Apr::CommandBufferObject encoding and the AMM
# executor in libkernel/Apr; AnyPS5 keeps the hardware packet layout (libkernel/Ampr/AmprPackets.hpp,
# covered by ampr_packet_tests), so that test is not built here.
