add_executable(ld-instantos src/main.cpp)

# ld-instantos needs the repo-root kernel headers (common/elf.hpp). In the
# in-tree (clang) build CMAKE_SOURCE_DIR is the repo root; when iUserApps is
# configured standalone (the GCC userland build) the caller passes the path via
# -DINSTANTOS_KERNEL_INCLUDE.
if(NOT DEFINED INSTANTOS_KERNEL_INCLUDE)
    set(INSTANTOS_KERNEL_INCLUDE ${CMAKE_SOURCE_DIR}/include)
endif()

target_include_directories(ld-instantos PRIVATE
    ${INSTANTOS_KERNEL_INCLUDE}
)

target_compile_options(ld-instantos PRIVATE
    ${USER_BASE_CXX_FLAGS}
    ${USER_TARGET_FLAGS}
    -fno-pic
    -mcmodel=large
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    ${SIZE_CXX_FLAGS}
)

target_link_options(ld-instantos PRIVATE
    -static
    -no-pie
    --gc-sections
    --build-id=none
    "SHELL:-z max-page-size=0x1000"
    ${INSTANTOS_LOADER_BASE_FLAG}
    -e
    _start
)

set_target_properties(ld-instantos PROPERTIES
    OUTPUT_NAME ld-instantos
    SUFFIX .so
)
