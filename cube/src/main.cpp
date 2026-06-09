#include <cstdint.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

constexpr std::uint32_t kSurfaceWidth = 640;
constexpr std::uint32_t kSurfaceHeight = 480;
constexpr std::uint32_t kFrameIntervalMs = 16;

constexpr std::uint32_t kColorBackground = 0x00091218U;
constexpr std::uint32_t kColorCube = 0x003ba4d8U;
constexpr std::uint32_t kColorCubeBright = 0x00d7f2e6U;

constexpr int kFixedShift = 14;
constexpr int kFixedOne = 1 << kFixedShift;

// Small incremental rotations.
// These are fixed-point approximations:
// cos(0.030) ~= 0.99955
// sin(0.030) ~= 0.03000
// cos(0.021) ~= 0.99978
// sin(0.021) ~= 0.02100
constexpr int kRotXCos = 16377;
constexpr int kRotXSin = 492;
constexpr int kRotYCos = 16380;
constexpr int kRotYSin = 344;

struct Vec3 {
    int x;
    int y;
    int z;
};

struct Point2 {
    int x;
    int y;
};

struct RotationState {
    int cosX;
    int sinX;
    int cosY;
    int sinY;
};

std::Handle connect_service(const char* name) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        const std::Handle handle = std::service_connect(name);
        if (handle != fail) {
            return handle;
        }
        std::yield();
    }

    return fail;
}

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

void clear_screen(std::uint32_t* pixels, std::uint32_t color) {
    if (!pixels) {
        return;
    }

    for (std::uint32_t y = 0; y < kSurfaceHeight; ++y) {
        for (std::uint32_t x = 0; x < kSurfaceWidth; ++x) {
            pixels[y * kSurfaceWidth + x] = color;
        }
    }
}

void put_pixel(std::uint32_t* pixels, int x, int y, std::uint32_t color) {
    if (!pixels) {
        return;
    }

    if (x < 0 || y < 0 ||
        x >= static_cast<int>(kSurfaceWidth) ||
        y >= static_cast<int>(kSurfaceHeight)) {
        return;
    }

    pixels[y * kSurfaceWidth + x] = color;
}

int abs_int(int value) {
    return value < 0 ? -value : value;
}

void draw_line(
    std::uint32_t* pixels,
    int x0,
    int y0,
    int x1,
    int y1,
    std::uint32_t color
) {
    int dx = abs_int(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs_int(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put_pixel(pixels, x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int fixed_mul(int a, int b) {
    return static_cast<int>(
        (static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)) >> kFixedShift
    );
}

void update_rotation(RotationState& rot) {
    const int oldCosX = rot.cosX;
    const int oldSinX = rot.sinX;
    const int oldCosY = rot.cosY;
    const int oldSinY = rot.sinY;

    rot.cosX = fixed_mul(oldCosX, kRotXCos) - fixed_mul(oldSinX, kRotXSin);
    rot.sinX = fixed_mul(oldSinX, kRotXCos) + fixed_mul(oldCosX, kRotXSin);

    rot.cosY = fixed_mul(oldCosY, kRotYCos) - fixed_mul(oldSinY, kRotYSin);
    rot.sinY = fixed_mul(oldSinY, kRotYCos) + fixed_mul(oldCosY, kRotYSin);
}

Vec3 rotate_vertex(const Vec3& v, const RotationState& rot) {
    // Rotate around X.
    Vec3 rx = {};
    rx.x = v.x;
    rx.y = fixed_mul(v.y, rot.cosX) - fixed_mul(v.z, rot.sinX);
    rx.z = fixed_mul(v.y, rot.sinX) + fixed_mul(v.z, rot.cosX);

    // Rotate around Y.
    Vec3 ry = {};
    ry.x = fixed_mul(rx.x, rot.cosY) + fixed_mul(rx.z, rot.sinY);
    ry.y = rx.y;
    ry.z = -fixed_mul(rx.x, rot.sinY) + fixed_mul(rx.z, rot.cosY);

    return ry;
}

Point2 project_vertex(const Vec3& v) {
    constexpr int cameraDistance = 420;
    constexpr int projectionScale = 260;

    int z = v.z + cameraDistance;
    if (z < 1) {
        z = 1;
    }

    Point2 out = {};
    out.x = static_cast<int>(kSurfaceWidth / 2) + (v.x * projectionScale) / z;
    out.y = static_cast<int>(kSurfaceHeight / 2) - (v.y * projectionScale) / z;
    return out;
}

void draw_cube(std::uint32_t* pixels, RotationState& rot) {
    clear_screen(pixels, kColorBackground);

    constexpr int size = 120;

    const Vec3 vertices[8] = {
        {-size, -size, -size},
        { size, -size, -size},
        { size,  size, -size},
        {-size,  size, -size},

        {-size, -size,  size},
        { size, -size,  size},
        { size,  size,  size},
        {-size,  size,  size},
    };

    const int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };

    Point2 projected[8];

    for (int i = 0; i < 8; ++i) {
        const Vec3 rotated = rotate_vertex(vertices[i], rot);
        projected[i] = project_vertex(rotated);
    }

    for (int i = 0; i < 12; ++i) {
        const int a = edges[i][0];
        const int b = edges[i][1];

        const std::uint32_t color = i < 4 ? kColorCubeBright : kColorCube;

        draw_line(
            pixels,
            projected[a].x,
            projected[a].y,
            projected[b].x,
            projected[b].y,
            color
        );
    }

    update_rotation(rot);
}
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    const std::Handle compositor = connect_service(std::services::graphics_compositor::NAME);
    if (compositor == fail) {
        write_str("[cube] FAIL service_connect graphics.compositor\n");
        return 1;
    }

    const std::Handle surface = std::surface_create(
        kSurfaceWidth,
        kSurfaceHeight,
        std::services::surfaces::FORMAT_BGRA8
    );
    if (surface == fail) {
        write_str("[cube] FAIL surface_create\n");
        std::close(compositor);
        return 1;
    }

    auto* pixels = static_cast<std::uint32_t*>(std::shared_map(surface));
    if (pixels == reinterpret_cast<std::uint32_t*>(fail) || pixels == nullptr) {
        write_str("[cube] FAIL shared_map(surface)\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle window = std::compositor_create_window(
        compositor,
        kSurfaceWidth,
        kSurfaceHeight,
        0
    );
    if (window == fail) {
        write_str("[cube] FAIL compositor_create_window\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    if (std::window_set_title(window, "Cube") == fail ||
        std::window_attach_surface(window, surface) == fail) {
        write_str("[cube] FAIL window setup\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle events = std::window_event_queue(window);
    if (events == fail) {
        write_str("[cube] FAIL window_event_queue\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    RotationState rotation = {};
    rotation.cosX = kFixedOne;
    rotation.sinX = 0;
    rotation.cosY = kFixedOne;
    rotation.sinY = 0;

    bool running = true;

    while (running) {
        for (;;) {
            std::Event event = {};
            if (std::event_poll(events, &event) == fail) {
                break;
            }

            if (event.type == std::EventType::Window &&
                event.window.action == std::WindowEventAction::CloseRequested) {
                running = false;
                break;
            }
        }

        draw_cube(pixels, rotation);

        if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
            write_str("[cube] FAIL surface_commit\n");
            break;
        }

        std::sleep(kFrameIntervalMs);
    }

    std::close(events);
    std::close(window);
    std::close(surface);
    std::close(compositor);
    return 0;
}
