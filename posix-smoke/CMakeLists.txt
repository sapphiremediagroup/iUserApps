add_user_app(posix-smoke)

function(add_posix_smoke_area target_name area_define)
    add_user_app_sources(${target_name} "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    target_compile_definitions(${target_name} PRIVATE
        POSIX_SMOKE_NAME="${target_name}"
        ${area_define}=1
    )
endfunction()

add_posix_smoke_area(posix-fs-smoke POSIX_SMOKE_FILESYSTEM)
add_posix_smoke_area(posix-mmap-smoke POSIX_SMOKE_MMAP)
add_posix_smoke_area(posix-ipc-smoke POSIX_SMOKE_IPC)
add_posix_smoke_area(posix-signal-smoke POSIX_SMOKE_SIGNALS)
add_posix_smoke_area(posix-thread-smoke POSIX_SMOKE_THREADS)
add_posix_smoke_area(posix-socket-smoke POSIX_SMOKE_NETWORK)
