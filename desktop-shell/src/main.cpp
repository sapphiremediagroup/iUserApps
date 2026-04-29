#include <cstdio.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 900;
constexpr std::uint32_t kSurfaceHeight = 620;

struct ShellPalette {
    std::uint32_t backgroundTop;
    std::uint32_t backgroundBottom;
    std::uint32_t panel;
    std::uint32_t accent;
    std::uint32_t accentSoft;
    std::uint32_t tileA;
    std::uint32_t tileB;
};

constexpr ShellPalette kPalettes[] = {
    { 0x00141b2dU, 0x001e3048U, 0x0026394dU, 0x00e39d3fU, 0x004d86b8U, 0x00344f71U, 0x00273c59U },
    { 0x00161d14U, 0x00293d29U, 0x002b4c40U, 0x00d6c256U, 0x0066956aU, 0x00365d4dU, 0x002d4d40U },
    { 0x0021141bU, 0x003e2432U, 0x00483248U, 0x00f07b86U, 0x007453a3U, 0x00533b62U, 0x0040324dU }
};

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

std::Handle connect_service(const char* name) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const std::Handle handle = std::service_connect(name);
        if (handle != fail) {
            return handle;
        }
        std::sleep(10);
    }

    return fail;
}

void fill_rect(
    std::uint32_t* pixels,
    std::uint32_t surfaceWidth,
    std::uint32_t surfaceHeight,
    int x,
    int y,
    int width,
    int height,
    std::uint32_t color
) {
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    int startX = x < 0 ? 0 : x;
    int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(surfaceWidth)) endX = static_cast<int>(surfaceWidth);
    if (endY > static_cast<int>(surfaceHeight)) endY = static_cast<int>(surfaceHeight);

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            pixels[drawY * surfaceWidth + drawX] = color;
        }
    }
}

void draw_gradient(std::uint32_t* pixels, const ShellPalette& palette) {
    for (std::uint32_t y = 0; y < kSurfaceHeight; ++y) {
        const std::uint32_t mix = (y * 255U) / (kSurfaceHeight - 1U);
        const std::uint32_t topR = (palette.backgroundTop >> 16) & 0xFFU;
        const std::uint32_t topG = (palette.backgroundTop >> 8) & 0xFFU;
        const std::uint32_t topB = palette.backgroundTop & 0xFFU;
        const std::uint32_t botR = (palette.backgroundBottom >> 16) & 0xFFU;
        const std::uint32_t botG = (palette.backgroundBottom >> 8) & 0xFFU;
        const std::uint32_t botB = palette.backgroundBottom & 0xFFU;

        const std::uint32_t red = ((topR * (255U - mix)) + (botR * mix)) / 255U;
        const std::uint32_t green = ((topG * (255U - mix)) + (botG * mix)) / 255U;
        const std::uint32_t blue = ((topB * (255U - mix)) + (botB * mix)) / 255U;
        const std::uint32_t rowColor = (red << 16) | (green << 8) | blue;

        for (std::uint32_t x = 0; x < kSurfaceWidth; ++x) {
            pixels[y * kSurfaceWidth + x] = rowColor;
        }
    }
}

void draw_desktop(std::uint32_t* pixels, std::uint32_t paletteIndex, bool focused, std::uint32_t activitySeed) {
    const ShellPalette& palette = kPalettes[paletteIndex % (sizeof(kPalettes) / sizeof(kPalettes[0]))];
    draw_gradient(pixels, palette);

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, 0, kSurfaceWidth, 56, palette.panel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, static_cast<int>(kSurfaceHeight) - 82, kSurfaceWidth, 82, palette.panel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 28, 88, 364, 212, palette.tileA);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 420, 88, 452, 132, palette.tileB);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 420, 244, 214, 260, palette.tileA);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 658, 244, 214, 260, palette.tileB);

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 28, 332, 364, 172, palette.accentSoft);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 358, 312, 18, palette.panel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 392, 224, 18, palette.panel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 426, 280, 18, palette.panel);

    const int indicatorWidth = 92 + static_cast<int>(activitySeed % 220U);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 460, indicatorWidth, 22, palette.accent);

    for (int i = 0; i < 5; ++i) {
        const int offset = 26 + (i * 72);
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, offset, static_cast<int>(kSurfaceHeight) - 58, 48, 34, palette.tileA);
    }

    fill_rect(
        pixels,
        kSurfaceWidth,
        kSurfaceHeight,
        static_cast<int>(kSurfaceWidth) - 168,
        static_cast<int>(kSurfaceHeight) - 62,
        132,
        36,
        focused ? palette.accent : palette.tileB
    );

    if (focused) {
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, 0, kSurfaceWidth, 6, palette.accent);
    } else {
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, 0, kSurfaceWidth, 6, palette.tileB);
    }

    const int pulseX = 440 + static_cast<int>((activitySeed * 13U) % 168U);
    const int pulseY = 274 + static_cast<int>((activitySeed * 7U) % 180U);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, pulseX, pulseY, 26, 26, palette.accent);
}

}

int main() {
    write_str("[desktop-shell] connecting to graphics.compositor\n");
    const std::Handle compositor = connect_service(std::services::graphics_compositor::NAME);
    if (compositor == fail) {
        write_str("[desktop-shell] FAIL service_connect graphics.compositor\n");
        return 1;
    }
    write_str("[desktop-shell] SUCCESS service_connect graphics.compositor\n");
    
    const std::Handle surface = std::surface_create(kSurfaceWidth, kSurfaceHeight, std::services::surfaces::FORMAT_BGRA8);
    if (surface == fail) {
        write_str("[desktop-shell] FAIL surface_create\n");
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS surface_create\n");
    
    auto* pixels = static_cast<std::uint32_t*>(std::shared_map(surface));
    if (pixels == reinterpret_cast<std::uint32_t*>(fail) || pixels == nullptr) {
        write_str("[desktop-shell] FAIL shared_map(surface)\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS shared_map(surface)\n");
    
    const std::Handle window = std::compositor_create_window(compositor, kSurfaceWidth, kSurfaceHeight, 0);
    if (window == fail) {
        write_str("[desktop-shell] FAIL compositor_create_window\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS compositor_create_window\n");
    
    if (std::window_set_title(window, "Desktop Shell") == fail) {
        write_str("[desktop-shell] FAIL window_set_title\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS window_set_title\n");
    
    if (std::window_attach_surface(window, surface) == fail) {
        write_str("[desktop-shell] FAIL window_attach_surface\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS window_attach_surface\n");
    
    const std::Handle events = std::window_event_queue(window);
    if (events == fail) {
        write_str("[desktop-shell] FAIL window_event_queue\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS window_event_queue\n");

    bool focused = false;
    std::uint32_t paletteIndex = 0;
    std::uint32_t activitySeed = 1;
    draw_desktop(pixels, paletteIndex, focused, activitySeed);
    if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
        write_str("[desktop-shell] FAIL initial surface_commit\n");
        std::close(events);
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }
    write_str("[desktop-shell] SUCCESS initial surface_commit\n");

    write_str("[desktop-shell] ready\n");

    for (;;) {
        std::Event event = {};
        if (std::event_wait(events, &event) == fail) {
            write_str("[desktop-shell] FAIL event_wait\n");
            break;
        }

        bool redraw = false;
        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                focused = true;
                redraw = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                focused = false;
                redraw = true;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                break;
            }
        } else if (event.type == std::EventType::Key && event.key.action == std::KeyEventAction::Press) {
            if (event.key.keycode == '\n' || event.key.keycode == '\r' || event.key.keycode == ' ') {
                paletteIndex++;
                activitySeed += 17;
                redraw = true;
            } else {
                activitySeed += 9;
                redraw = true;
            }
        }

        if (!redraw) {
            continue;
        }

        draw_desktop(pixels, paletteIndex, focused, activitySeed);
        if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
            write_str("[desktop-shell] FAIL surface_commit update\n");
            break;
        }
    }

    std::close(events);
    std::close(window);
    std::close(surface);
    std::close(compositor);
    return 0;
}
