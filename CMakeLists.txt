cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
project(iUserApps C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(USER_CXX_FLAGS
    ${BASE_CXX_FLAGS}
    -target x86_64-unknown-windows
)

function(add_user_app target_name)
    file(GLOB app_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_executable(${target_name}.exe ${app_sources})
    target_compile_options(${target_name}.exe PRIVATE ${USER_CXX_FLAGS})
    target_link_libraries(${target_name}.exe PRIVATE ilibcxx::ilibcxx)
    target_link_options(${target_name}.exe PRIVATE
        ${EFI_LINK_OPTIONS}
        -Wl,/base:0x00007FFFFFE00000
        -Wl,-subsystem:console
        -Wl,-entry:userland_entry
        -T${CMAKE_SOURCE_DIR}/linker.ld
    )
endfunction()

add_subdirectory(outside/ilibcxx)

add_subdirectory(hello)
add_subdirectory(input-manager)
add_subdirectory(graphics-compositor)
add_subdirectory(storage-manager)
add_subdirectory(process-manager)
add_subdirectory(font-manager)
add_subdirectory(session-manager)
add_subdirectory(login)
add_subdirectory(desktop-shell)
