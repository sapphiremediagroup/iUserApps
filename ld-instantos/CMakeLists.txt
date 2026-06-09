add_executable(ld-instantos src/main.cpp)

target_include_directories(ld-instantos PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(ld-instantos PRIVATE
    ${USER_BASE_CXX_FLAGS}
    -target x86_64-unknown-elf
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
    --image-base=0x0000500000000000
    -e
    _start
)

set_target_properties(ld-instantos PROPERTIES
    OUTPUT_NAME ld-instantos
    SUFFIX .so
)
