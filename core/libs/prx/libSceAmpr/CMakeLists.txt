add_library(libSceAmpr SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceAmpr PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libSceAmpr PRIVATE libkernel libc)
configure_windows_unwind(libSceAmpr)

# Upstream's tests/GuestAmpr.cpp targets its private Apr::CommandBufferObject encoding and the AMM
# executor in libkernel/Apr; AnyPS5 keeps the hardware packet layout (libkernel/Ampr/AmprPackets.hpp,
# covered by ampr_packet_tests), so that test is not built here.
if(BUILD_TESTING)
    # _Thrd_join and _Thrd_sleep come from libkernel's Pthread/src/CxxThreads.cpp here.
    add_test_executable(guest_ampr_threads_tests ${LIBS_INCLUDE_DIR}/tests/GuestAmprThreads.cpp)
    target_include_directories(guest_ampr_threads_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_ampr_threads_tests PRIVATE libSceAmpr libkernel libc)
    configure_windows_unwind(guest_ampr_threads_tests)
    add_test(NAME guest_ampr_threads COMMAND guest_ampr_threads_tests)
    if(WIN32)
        set_tests_properties(guest_ampr_threads PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAmpr>")
    endif()
endif()
