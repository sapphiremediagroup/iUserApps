add_user_app(graphics-compositor)

get_target_property(GRAPHICS_COMPOSITOR_COMPILE_OPTIONS graphics-compositor COMPILE_OPTIONS)
list(REMOVE_ITEM GRAPHICS_COMPOSITOR_COMPILE_OPTIONS -fno-inline -Oz)
set_target_properties(graphics-compositor PROPERTIES
    COMPILE_OPTIONS "${GRAPHICS_COMPOSITOR_COMPILE_OPTIONS}"
)

target_compile_options(graphics-compositor PRIVATE
    -O2
    -finline-functions
)
