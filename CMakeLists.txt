cmake_minimum_required(VERSION 3.20)
project(iUserApps C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_LINK_DEPENDS_USE_LINKER FALSE)
set(CMAKE_SKIP_RPATH TRUE)

# --- Userland toolchain selection (P5.5 Stage 4b) -------------------------
# Default: clang cross (-target x86_64-unknown-elf) linking with ld.lld.
# INSTANTOS_USERLAND_GCC=ON builds the libinstant userland with the GCC cross
# (CMAKE_CXX_COMPILER=x86_64-unknown-instantos-g++, set by a toolchain file)
# linking with GNU ld (INSTANTOS_GCC_LD). GCC is already the instantos target so
# it needs no -target, and it uses -Os rather than clang's -Oz.
option(INSTANTOS_USERLAND_GCC "Build the libinstant userland with the GCC cross toolchain" OFF)
if(INSTANTOS_USERLAND_GCC)
    set(USER_TARGET_FLAGS "")
    if(NOT INSTANTOS_GCC_LD)
        set(INSTANTOS_GCC_LD x86_64-unknown-instantos-ld)
    endif()
    set(_USER_LD "${INSTANTOS_GCC_LD}")
    set(_USER_SIZE_OPT -Os)
    # GNU ld places the fixed-address loader with -Ttext-segment (lld uses
    # --image-base for the same effect).
    set(INSTANTOS_LOADER_BASE_FLAG -Ttext-segment=0x0000500000000000)
    # Standalone build: emit binaries into out/ so executables (e.g.
    # input-manager) do not collide with the per-target build subdirs that sit
    # directly under CMAKE_BINARY_DIR.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
else()
    set(USER_TARGET_FLAGS -target x86_64-unknown-elf)
    set(_USER_LD ld.lld)
    set(_USER_SIZE_OPT -Oz)
    set(INSTANTOS_LOADER_BASE_FLAG --image-base=0x0000500000000000)
endif()

set(CMAKE_CXX_LINK_EXECUTABLE "${_USER_LD} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "${_USER_LD} -shared <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# SIZE_CXX_FLAGS is normally injected by the parent (kernel) CMakeLists; when
# iUserApps is configured standalone (the GCC userland build) provide it here.
if(NOT DEFINED SIZE_CXX_FLAGS)
    set(SIZE_CXX_FLAGS ${_USER_SIZE_OPT} -ffunction-sections -fdata-sections -fomit-frame-pointer)
endif()

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
    ${USER_TARGET_FLAGS}
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
    add_user_app_sources(${target_name} ${app_sources})
endfunction()

# Recovered from the transcript reads sidecar: the lost commit split the body of
# add_user_app into this helper so callers (e.g. posix-smoke) can build several
# targets from an explicit source list rather than a src/*.cpp glob.
function(add_user_app_sources target_name)
    add_executable(${target_name} ${ARGN} $<TARGET_OBJECTS:instant_crt0>)
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
add_subdirectory(ui-demo)
# posix-smoke / net-smoke are conformance test apps (not runtime substrate and
# not shipped in the os-testsuite ISO). posix-smoke uses a few declarations
# (mkfifo/EWOULDBLOCK) that the clang header set exposes but GCC's does not, so
# skip them in the GCC userland build.
if(NOT INSTANTOS_USERLAND_GCC)
    add_subdirectory(posix-smoke)
    add_subdirectory(net-smoke)
endif()
