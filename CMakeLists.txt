cmake_minimum_required(VERSION 3.20)
project(iUserApps C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_LINK_DEPENDS_USE_LINKER FALSE)
set(CMAKE_SKIP_RPATH TRUE)

set(CMAKE_CXX_LINK_EXECUTABLE "ld.lld <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "ld.lld -shared <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(USER_BASE_CXX_FLAGS
    -fno-rtti
    -fno-exceptions
    -fshort-wchar
    -ffreestanding
    -fno-threadsafe-statics
    -fno-inline
    -fno-use-cxa-atexit
    -fno-omit-frame-pointer
    -nodefaultlibs
    -nostdinc++
)

set(USER_CXX_FLAGS
    ${USER_BASE_CXX_FLAGS}
    -target x86_64-unknown-elf
    -fPIC
)

set(USER_LINK_OPTIONS
    --gc-sections
    --build-id=none
    --hash-style=sysv
    "SHELL:-z max-page-size=0x1000"
)

function(add_user_app target_name)
    file(GLOB app_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_executable(${target_name} ${app_sources} $<TARGET_OBJECTS:instant_crt0>)
    target_compile_options(${target_name} PRIVATE ${USER_CXX_FLAGS} ${SIZE_CXX_FLAGS})
    target_link_libraries(${target_name} PRIVATE ilibcxx::ilibcxx)
    target_link_options(${target_name} PRIVATE
        ${USER_LINK_OPTIONS}
        -pie
        -e _start
        --dynamic-linker
        /lib/ld-instantos.so
        "SHELL:-z now"
    )
    set_target_properties(${target_name} PROPERTIES SUFFIX "")
endfunction()

function(add_instant_window_app target_name)
    file(GLOB app_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_executable(${target_name} ${app_sources} $<TARGET_OBJECTS:instant_window_crt0>)
    target_compile_options(${target_name} PRIVATE ${USER_CXX_FLAGS} ${SIZE_CXX_FLAGS})
    target_link_libraries(${target_name} PRIVATE ilibcxx::ilibcxx)
    target_link_options(${target_name} PRIVATE
        ${USER_LINK_OPTIONS}
        -pie
        -e _start
        --dynamic-linker
        /lib/ld-instantos.so
        "SHELL:-z now"
    )
    set_target_properties(${target_name} PROPERTIES SUFFIX "")
endfunction()

add_subdirectory(outside/ilibcxx)
add_subdirectory(ld-instantos)

add_subdirectory(input-manager)
add_subdirectory(graphics-compositor)
add_subdirectory(storage-manager)
add_subdirectory(process-manager)
add_subdirectory(network-manager)
add_subdirectory(font-manager)
add_subdirectory(session-manager)
add_subdirectory(login)
add_subdirectory(desktop-shell)
add_subdirectory(background-switcher)
add_subdirectory(file-browser)
add_subdirectory(terminal)
add_subdirectory(cube)
add_subdirectory(posix-smoke)
add_subdirectory(net-smoke)
