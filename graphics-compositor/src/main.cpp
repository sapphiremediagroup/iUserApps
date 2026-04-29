#include <cstdint.hpp>
#include <cstring.hpp>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kFrameIntervalMs = 16;
constexpr std::uint32_t kCompositorBufferCount = 3;
constexpr std::uint32_t kBackground = 0x00101820;
constexpr std::uint32_t kWindowBackground = 0x00d7dde5;
constexpr std::uint32_t kWindowBorder = 0x00283542;
constexpr std::uint32_t kWindowBorderFocused = 0x006da9ff;
constexpr std::uint32_t kTitleBar = 0x00455f7a;
constexpr std::uint32_t kTitleBarFocused = 0x00367dd6;
constexpr std::uint32_t kButtonClose = 0x00d14d4d;
constexpr std::uint32_t kButtonMax = 0x00c89b2b;
constexpr std::uint32_t kButtonMin = 0x004aa56b;
constexpr std::uint32_t kResizeGrip = 0x00556777;
constexpr std::uint32_t kCursorFill = 0x00f5f7fa;
constexpr std::uint32_t kCursorOutline = 0x00000000;
constexpr int kCursorSize = 7;
constexpr int kBorder = 2;
constexpr int kTitleBarHeight = 24;
constexpr int kButtonSize = 12;
constexpr int kButtonGap = 6;
constexpr int kResizeGripSize = 14;
constexpr std::uint64_t kMaxWindows = 64;
constexpr std::uint64_t kMaxSurfaceCache = 64;

struct CursorState {
    int x;
    int y;
};

struct Rect {
    int x;
    int y;
    int width;
    int height;
};

struct DragState {
    std::uint64_t windowId;
    bool moving;
    bool resizing;
    int grabOffsetX;
    int grabOffsetY;
    int anchorX;
    int anchorY;
    int startWidth;
    int startHeight;
};

struct FramebufferView {
    std::uint32_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};

struct RGBATexel {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

struct RenderBuffer {
    RGBATexel* pixels;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};

struct TripleBufferState {
    RenderBuffer buffers[kCompositorBufferCount];
    std::uint32_t nextIndex;
};

struct SurfaceCacheEntry {
    bool valid;
    std::uint64_t id;
    std::SurfaceInfo info;
};

static std::WindowInfo gWindowsScratch[kMaxWindows];
static SurfaceCacheEntry gSurfaceCache[kMaxSurfaceCache];

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

bool decode_event_message(const std::IPCMessage& message, std::Event* event) {
    return std::event_from_message(message, event);
}

RGBATexel rgba_from_rgb(std::uint32_t color) {
    RGBATexel pixel = {};
    pixel.r = static_cast<std::uint8_t>((color >> 16) & 0xFFU);
    pixel.g = static_cast<std::uint8_t>((color >> 8) & 0xFFU);
    pixel.b = static_cast<std::uint8_t>(color & 0xFFU);
    pixel.a = 0xFFU;
    return pixel;
}

std::uint32_t rgb_to_screen(const RGBATexel& pixel) {
    return (static_cast<std::uint32_t>(pixel.r) << 16) |
           (static_cast<std::uint32_t>(pixel.g) << 8) |
           static_cast<std::uint32_t>(pixel.b);
}

void clear_buffer(const RenderBuffer& buffer, const RGBATexel& color) {
    for (std::uint32_t y = 0; y < buffer.height; ++y) {
        for (std::uint32_t x = 0; x < buffer.width; ++x) {
            buffer.pixels[y * buffer.pitch + x] = color;
        }
    }
}

void fill_rect(const RenderBuffer& buffer, int x, int y, int width, int height, const RGBATexel& color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    int startX = x < 0 ? 0 : x;
    int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(buffer.width)) {
        endX = static_cast<int>(buffer.width);
    }
    if (endY > static_cast<int>(buffer.height)) {
        endY = static_cast<int>(buffer.height);
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            buffer.pixels[drawY * buffer.pitch + drawX] = color;
        }
    }
}

bool cursor_mask(int localX, int localY) {
    return localX == 3 || localY == 3 || localX == localY || localX + localY == (kCursorSize - 1);
}

void draw_cursor(const RenderBuffer& buffer, const CursorState& cursor) {
    const RGBATexel fill = rgba_from_rgb(kCursorFill);
    const RGBATexel outline = rgba_from_rgb(kCursorOutline);

    for (int y = 0; y < kCursorSize; ++y) {
        for (int x = 0; x < kCursorSize; ++x) {
            const int drawX = cursor.x + x;
            const int drawY = cursor.y + y;
            if (drawX < 0 || drawY < 0 ||
                drawX >= static_cast<int>(buffer.width) || drawY >= static_cast<int>(buffer.height)) {
                continue;
            }

            if (cursor_mask(x, y)) {
                buffer.pixels[drawY * buffer.pitch + drawX] = (x == 3 || y == 3) ? outline : fill;
            }
        }
    }
}

Rect cursor_rect(const CursorState& cursor) {
    return { cursor.x, cursor.y, kCursorSize, kCursorSize };
}

bool rect_is_empty(const Rect& rect) {
    return rect.width <= 0 || rect.height <= 0;
}

Rect union_rect(const Rect& a, const Rect& b) {
    if (rect_is_empty(a)) {
        return b;
    }
    if (rect_is_empty(b)) {
        return a;
    }

    const int left = a.x < b.x ? a.x : b.x;
    const int top = a.y < b.y ? a.y : b.y;
    const int rightA = a.x + a.width;
    const int rightB = b.x + b.width;
    const int bottomA = a.y + a.height;
    const int bottomB = b.y + b.height;
    const int right = rightA > rightB ? rightA : rightB;
    const int bottom = bottomA > bottomB ? bottomA : bottomB;
    return { left, top, right - left, bottom - top };
}

Rect clamp_rect(const Rect& rect, std::uint32_t width, std::uint32_t height) {
    Rect clamped = rect;
    if (clamped.x < 0) {
        clamped.width += clamped.x;
        clamped.x = 0;
    }
    if (clamped.y < 0) {
        clamped.height += clamped.y;
        clamped.y = 0;
    }

    const int maxWidth = static_cast<int>(width);
    const int maxHeight = static_cast<int>(height);
    if (clamped.x + clamped.width > maxWidth) {
        clamped.width = maxWidth - clamped.x;
    }
    if (clamped.y + clamped.height > maxHeight) {
        clamped.height = maxHeight - clamped.y;
    }
    if (clamped.width < 0) {
        clamped.width = 0;
    }
    if (clamped.height < 0) {
        clamped.height = 0;
    }
    return clamped;
}

void present_rect_to_framebuffer(const RenderBuffer& source, const FramebufferView& target, const Rect& rect) {
    const Rect clipped = clamp_rect(rect, source.width < target.width ? source.width : target.width,
        source.height < target.height ? source.height : target.height);
    if (rect_is_empty(clipped)) {
        return;
    }

    for (int y = 0; y < clipped.height; ++y) {
        const int srcY = clipped.y + y;
        for (int x = 0; x < clipped.width; ++x) {
            const int srcX = clipped.x + x;
            target.pixels[srcY * target.pitch + srcX] = rgb_to_screen(source.pixels[srcY * source.pitch + srcX]);
        }
    }

    std::fb_flush(
        static_cast<std::uint32_t>(clipped.x),
        static_cast<std::uint32_t>(clipped.y),
        static_cast<std::uint32_t>(clipped.width),
        static_cast<std::uint32_t>(clipped.height)
    );
}

void draw_cursor_to_framebuffer(const FramebufferView& fb, const CursorState& cursor) {
    const std::uint32_t fill = rgb_to_screen(rgba_from_rgb(kCursorFill));
    const std::uint32_t outline = rgb_to_screen(rgba_from_rgb(kCursorOutline));

    for (int y = 0; y < kCursorSize; ++y) {
        for (int x = 0; x < kCursorSize; ++x) {
            const int drawX = cursor.x + x;
            const int drawY = cursor.y + y;
            if (drawX < 0 || drawY < 0 ||
                drawX >= static_cast<int>(fb.width) || drawY >= static_cast<int>(fb.height)) {
                continue;
            }

            if (cursor_mask(x, y)) {
                fb.pixels[drawY * fb.pitch + drawX] = (x == 3 || y == 3) ? outline : fill;
            }
        }
    }
}

void present_cursor_move(const RenderBuffer& scene, const FramebufferView& fb, const CursorState& oldCursor, const CursorState& newCursor) {
    const Rect dirty = clamp_rect(union_rect(cursor_rect(oldCursor), cursor_rect(newCursor)), fb.width, fb.height);
    if (rect_is_empty(dirty)) {
        return;
    }

    const RGBATexel fillPixel = rgba_from_rgb(kCursorFill);
    const RGBATexel outlinePixel = rgba_from_rgb(kCursorOutline);
    const std::uint32_t fill = rgb_to_screen(fillPixel);
    const std::uint32_t outline = rgb_to_screen(outlinePixel);

    for (int y = 0; y < dirty.height; ++y) {
        const int drawY = dirty.y + y;
        for (int x = 0; x < dirty.width; ++x) {
            const int drawX = dirty.x + x;
            std::uint32_t color = rgb_to_screen(scene.pixels[drawY * scene.pitch + drawX]);

            const int cursorLocalX = drawX - newCursor.x;
            const int cursorLocalY = drawY - newCursor.y;
            if (cursorLocalX >= 0 && cursorLocalY >= 0 &&
                cursorLocalX < kCursorSize && cursorLocalY < kCursorSize &&
                cursor_mask(cursorLocalX, cursorLocalY)) {
                color = (cursorLocalX == 3 || cursorLocalY == 3) ? outline : fill;
            }

            fb.pixels[drawY * fb.pitch + drawX] = color;
        }
    }

    std::fb_flush(
        static_cast<std::uint32_t>(dirty.x),
        static_cast<std::uint32_t>(dirty.y),
        static_cast<std::uint32_t>(dirty.width),
        static_cast<std::uint32_t>(dirty.height)
    );
}

void move_cursor(const FramebufferView& fb, CursorState* cursor, int x, int y) {
    if (!cursor) {
        return;
    }

    int maxX = static_cast<int>(fb.width) - kCursorSize;
    int maxY = static_cast<int>(fb.height) - kCursorSize;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    cursor->x = x < 0 ? 0 : (x > maxX ? maxX : x);
    cursor->y = y < 0 ? 0 : (y > maxY ? maxY : y);
}

bool build_hello_reply(const std::IPCMessage& message, std::services::graphics_compositor::HelloReply* reply) {
    if (!reply) {
        return false;
    }

    std::services::graphics_compositor::HelloRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    reply->header.version = std::services::graphics_compositor::VERSION;
    reply->header.opcode = static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::Hello);
    reply->status = std::services::STATUS_OK;
    std::strncpy(reply->service_name, std::services::graphics_compositor::NAME, sizeof(reply->service_name) - 1);

    if (request.header.version != std::services::graphics_compositor::VERSION) {
        reply->status = std::services::STATUS_BAD_VERSION;
    } else if (request.header.opcode != static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::Hello)) {
        reply->status = std::services::STATUS_BAD_OPCODE;
    }

    return true;
}

bool point_in_rect(int px, int py, int x, int y, int width, int height) {
    return px >= x && py >= y && px < (x + width) && py < (y + height);
}

int frame_width(const std::WindowInfo& window) {
    return window.width + (kBorder * 2);
}

int frame_height(const std::WindowInfo& window) {
    return window.height + kTitleBarHeight + kBorder;
}

bool is_window_visible(const std::WindowInfo& window) {
    return (window.state & (std::WindowStateMinimized | std::WindowStateClosed)) == 0;
}

SurfaceCacheEntry* find_surface_cache(SurfaceCacheEntry* cache, std::uint64_t surfaceId) {
    if (surfaceId == 0) {
        return nullptr;
    }

    for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
        if (cache[i].valid && cache[i].id == surfaceId) {
            return &cache[i];
        }
    }

    return nullptr;
}

void remember_surface(SurfaceCacheEntry* cache, const std::SurfaceInfo& info) {
    SurfaceCacheEntry* entry = find_surface_cache(cache, info.id);
    if (!entry) {
        for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
            if (!cache[i].valid) {
                entry = &cache[i];
                break;
            }
        }
    }
    if (!entry) {
        entry = &cache[0];
    }

    entry->valid = true;
    entry->id = info.id;
    entry->info = info;
}

bool pump_surface_updates(SurfaceCacheEntry* cache) {
    bool changed = false;
    for (;;) {
        std::SurfaceInfo surface = {};
        if (std::surface_poll(&surface) == fail) {
            break;
        }

        remember_surface(cache, surface);
        changed = true;
    }
    return changed;
}

void draw_window_frame(const RenderBuffer& buffer, const std::WindowInfo& window) {
    const bool focused = (window.state & std::WindowStateFocused) != 0;
    const int frameX = window.x;
    const int frameY = window.y;
    const int totalWidth = frame_width(window);
    const int totalHeight = frame_height(window);

    fill_rect(buffer, frameX, frameY, totalWidth, totalHeight, rgba_from_rgb(focused ? kWindowBorderFocused : kWindowBorder));
    fill_rect(buffer, frameX + kBorder, frameY + kBorder, window.width, kTitleBarHeight - kBorder, rgba_from_rgb(focused ? kTitleBarFocused : kTitleBar));
    fill_rect(buffer, frameX + kBorder, frameY + kTitleBarHeight, window.width, window.height, rgba_from_rgb(kWindowBackground));

    const int buttonY = frameY + 6;
    const int closeX = frameX + totalWidth - kButtonGap - kButtonSize;
    const int maxX = closeX - kButtonGap - kButtonSize;
    const int minX = maxX - kButtonGap - kButtonSize;
    fill_rect(buffer, minX, buttonY, kButtonSize, kButtonSize, rgba_from_rgb(kButtonMin));
    fill_rect(buffer, maxX, buttonY, kButtonSize, kButtonSize, rgba_from_rgb(kButtonMax));
    fill_rect(buffer, closeX, buttonY, kButtonSize, kButtonSize, rgba_from_rgb(kButtonClose));

    const int gripX = frameX + totalWidth - kBorder - kResizeGripSize;
    const int gripY = frameY + totalHeight - kBorder - kResizeGripSize;
    fill_rect(buffer, gripX, gripY, kResizeGripSize, kResizeGripSize, rgba_from_rgb(kResizeGrip));
}

void blit_window_surface(const RenderBuffer& buffer, const std::WindowInfo& window, const SurfaceCacheEntry* surfaceEntry) {
    if (!surfaceEntry || !surfaceEntry->valid) {
        return;
    }

    const auto* src = reinterpret_cast<const std::uint32_t*>(surfaceEntry->info.address);
    if (!src) {
        return;
    }

    const int destX = window.x + kBorder;
    const int destY = window.y + kTitleBarHeight;
    const std::uint32_t sourcePitch = surfaceEntry->info.pitch >= 4
        ? surfaceEntry->info.pitch / 4
        : surfaceEntry->info.width;
    const std::uint32_t copyWidth = surfaceEntry->info.width < static_cast<std::uint32_t>(window.width)
        ? surfaceEntry->info.width
        : static_cast<std::uint32_t>(window.width);
    const std::uint32_t copyHeight = surfaceEntry->info.height < static_cast<std::uint32_t>(window.height)
        ? surfaceEntry->info.height
        : static_cast<std::uint32_t>(window.height);

    for (std::uint32_t y = 0; y < copyHeight; ++y) {
        const int drawY = destY + static_cast<int>(y);
        if (drawY < 0 || drawY >= static_cast<int>(buffer.height)) {
            continue;
        }

        for (std::uint32_t x = 0; x < copyWidth; ++x) {
            const int drawX = destX + static_cast<int>(x);
            if (drawX < 0 || drawX >= static_cast<int>(buffer.width)) {
                continue;
            }

            buffer.pixels[drawY * buffer.pitch + drawX] = rgba_from_rgb(src[y * sourcePitch + x]);
        }
    }
}

std::uint64_t fetch_windows(std::WindowInfo* windows, std::uint64_t capacity) {
    const std::uint64_t count = std::compositor_list_windows(windows, capacity);
    if (count == fail) {
        return 0;
    }
    return count;
}

void redraw_scene(const RenderBuffer& buffer, SurfaceCacheEntry* cache) {
    clear_buffer(buffer, rgba_from_rgb(kBackground));

    std::memset(gWindowsScratch, 0, sizeof(gWindowsScratch));
    const std::uint64_t count = fetch_windows(gWindowsScratch, kMaxWindows);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!is_window_visible(gWindowsScratch[i])) {
            continue;
        }

        draw_window_frame(buffer, gWindowsScratch[i]);
        blit_window_surface(buffer, gWindowsScratch[i], find_surface_cache(cache, gWindowsScratch[i].surfaceID));
    }

}

void present_to_framebuffer(const RenderBuffer& source, const FramebufferView& target) {
    const std::uint32_t copyWidth = source.width < target.width ? source.width : target.width;
    const std::uint32_t copyHeight = source.height < target.height ? source.height : target.height;

    for (std::uint32_t y = 0; y < copyHeight; ++y) {
        for (std::uint32_t x = 0; x < copyWidth; ++x) {
            target.pixels[y * target.pitch + x] = rgb_to_screen(source.pixels[y * source.pitch + x]);
        }
    }

    std::fb_flush(0, 0, copyWidth, copyHeight);
}

bool initialize_triple_buffers(TripleBufferState* state, std::uint32_t width, std::uint32_t height) {
    if (!state || width == 0 || height == 0) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::uint32_t i = 0; i < kCompositorBufferCount; ++i) {
        state->buffers[i].pixels = new (std::nothrow) RGBATexel[pixelCount];
        if (state->buffers[i].pixels == nullptr) {
            return false;
        }
        state->buffers[i].width = width;
        state->buffers[i].height = height;
        state->buffers[i].pitch = width;
        clear_buffer(state->buffers[i], rgba_from_rgb(kBackground));
    }

    state->nextIndex = 0;
    return true;
}

RenderBuffer& acquire_render_buffer(TripleBufferState* state) {
    RenderBuffer& buffer = state->buffers[state->nextIndex];
    state->nextIndex = (state->nextIndex + 1U) % kCompositorBufferCount;
    return buffer;
}

const std::WindowInfo* top_window_at(const std::WindowInfo* windows, std::uint64_t count, int x, int y) {
    for (std::uint64_t i = count; i > 0; --i) {
        const std::WindowInfo& window = windows[i - 1];
        if (!is_window_visible(window)) {
            continue;
        }

        if (point_in_rect(x, y, window.x, window.y, frame_width(window), frame_height(window))) {
            return &window;
        }
    }

    return nullptr;
}

bool pointer_on_close(const std::WindowInfo& window, int x, int y) {
    const int closeX = window.x + frame_width(window) - kButtonGap - kButtonSize;
    return point_in_rect(x, y, closeX, window.y + 6, kButtonSize, kButtonSize);
}

bool pointer_on_maximize(const std::WindowInfo& window, int x, int y) {
    const int closeX = window.x + frame_width(window) - kButtonGap - kButtonSize;
    const int maxX = closeX - kButtonGap - kButtonSize;
    return point_in_rect(x, y, maxX, window.y + 6, kButtonSize, kButtonSize);
}

bool pointer_on_minimize(const std::WindowInfo& window, int x, int y) {
    const int closeX = window.x + frame_width(window) - kButtonGap - kButtonSize;
    const int maxX = closeX - kButtonGap - kButtonSize;
    const int minX = maxX - kButtonGap - kButtonSize;
    return point_in_rect(x, y, minX, window.y + 6, kButtonSize, kButtonSize);
}

bool pointer_on_resize_grip(const std::WindowInfo& window, int x, int y) {
    return point_in_rect(
        x,
        y,
        window.x + frame_width(window) - kBorder - kResizeGripSize,
        window.y + frame_height(window) - kBorder - kResizeGripSize,
        kResizeGripSize,
        kResizeGripSize
    );
}

bool pointer_on_titlebar(const std::WindowInfo& window, int x, int y) {
    return point_in_rect(x, y, window.x + kBorder, window.y + kBorder, window.width, kTitleBarHeight - kBorder);
}

void begin_move(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY) {
    drag->windowId = window.id;
    drag->moving = true;
    drag->resizing = false;
    drag->grabOffsetX = pointerX - window.x;
    drag->grabOffsetY = pointerY - window.y;
}

void begin_resize(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY) {
    drag->windowId = window.id;
    drag->moving = false;
    drag->resizing = true;
    drag->anchorX = pointerX;
    drag->anchorY = pointerY;
    drag->startWidth = window.width;
    drag->startHeight = window.height;
}

void clear_drag(DragState* drag) {
    drag->windowId = 0;
    drag->moving = false;
    drag->resizing = false;
}

bool handle_pointer_event(const std::Event& event, DragState* drag, std::uint16_t* buttons) {
    if (event.type != std::EventType::Pointer) {
        return false;
    }

    bool redraw = false;
    const std::uint16_t previousButtons = *buttons;
    *buttons = event.pointer.buttons;
    const bool leftWasDown = (previousButtons & 0x1u) != 0;
    const bool leftIsDown = (event.pointer.buttons & 0x1u) != 0;

    if (drag->moving && leftIsDown) {
        if (std::compositor_move_window(
                drag->windowId,
                event.pointer.x - drag->grabOffsetX,
                event.pointer.y - drag->grabOffsetY) != fail) {
            redraw = true;
        }
    } else if (drag->resizing && leftIsDown) {
        int newWidth = drag->startWidth + (event.pointer.x - drag->anchorX);
        int newHeight = drag->startHeight + (event.pointer.y - drag->anchorY);
        if (newWidth < 64) newWidth = 64;
        if (newHeight < 48) newHeight = 48;
        if (std::compositor_resize_window(drag->windowId, newWidth, newHeight) != fail) {
            redraw = true;
        }
    }

    if (!leftWasDown && leftIsDown) {
        std::memset(gWindowsScratch, 0, sizeof(gWindowsScratch));
        const std::uint64_t count = fetch_windows(gWindowsScratch, kMaxWindows);
        const std::WindowInfo* window = top_window_at(gWindowsScratch, count, event.pointer.x, event.pointer.y);
        if (window) {
            std::compositor_focus_window(window->id);
            redraw = true;

            if (pointer_on_close(*window, event.pointer.x, event.pointer.y)) {
                std::compositor_control_window(window->id, std::WindowControlAction::Close);
                clear_drag(drag);
                return true;
            }
            if (pointer_on_maximize(*window, event.pointer.x, event.pointer.y)) {
                const std::WindowControlAction action =
                    (window->state & std::WindowStateMaximized) != 0
                    ? std::WindowControlAction::Restore
                    : std::WindowControlAction::Maximize;
                std::compositor_control_window(window->id, action);
                clear_drag(drag);
                return true;
            }
            if (pointer_on_minimize(*window, event.pointer.x, event.pointer.y)) {
                std::compositor_control_window(window->id, std::WindowControlAction::Minimize);
                clear_drag(drag);
                return true;
            }
            if (pointer_on_resize_grip(*window, event.pointer.x, event.pointer.y)) {
                begin_resize(drag, *window, event.pointer.x, event.pointer.y);
                return true;
            }
            if (pointer_on_titlebar(*window, event.pointer.x, event.pointer.y)) {
                begin_move(drag, *window, event.pointer.x, event.pointer.y);
                return true;
            }
        }
    }

    if (leftWasDown && !leftIsDown) {
        clear_drag(drag);
    }

    return redraw;
}
}

int main() {
    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_str("[graphics.compositor] queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::graphics_compositor::NAME, queue) == fail) {
        write_str("[graphics.compositor] service_register failed\n");
        std::close(queue);
        return 1;
    }

    std::FBInfo info = {};
    if (std::fb_info(&info) == fail) {
        write_str("[graphics.compositor] fb_info failed\n");
        std::close(queue);
        return 1;
    }

    auto* pixels = static_cast<std::uint32_t*>(std::fb_map());
    if (pixels == reinterpret_cast<std::uint32_t*>(fail)) {
        pixels = reinterpret_cast<std::uint32_t*>(info.addr);
    }
    if (pixels == nullptr || pixels == reinterpret_cast<std::uint32_t*>(fail)) {
        write_str("[graphics.compositor] fb_map failed\n");
        std::close(queue);
        return 1;
    }

    const FramebufferView fb = {
        pixels,
        info.width,
        info.height,
        info.pitch
    };

    TripleBufferState buffers = {};
    if (!initialize_triple_buffers(&buffers, info.width, info.height)) {
        write_str("[graphics.compositor] triple buffer allocation failed\n");
        std::close(queue);
        return 1;
    }

    CursorState cursor = {};
    cursor.x = static_cast<int>(info.width / 2);
    cursor.y = static_cast<int>(info.height / 2);

    std::memset(gSurfaceCache, 0, sizeof(gSurfaceCache));
    DragState drag = {};
    std::uint16_t pointerButtons = 0;

    RenderBuffer* sceneBuffer = &acquire_render_buffer(&buffers);
    redraw_scene(*sceneBuffer, gSurfaceCache);
    present_to_framebuffer(*sceneBuffer, fb);
    present_cursor_move(*sceneBuffer, fb, cursor, cursor);

    write_str("[graphics.compositor] ready\n");

    auto handle_message = [&](const std::IPCMessage& message, bool* sceneDirty) {
        if ((message.flags & std::IPC_MESSAGE_EVENT) != 0) {
            std::Event event = {};
            if (decode_event_message(message, &event) && event.type == std::EventType::Pointer) {
                const CursorState previousCursor = cursor;
                move_cursor(fb, &cursor, event.pointer.x, event.pointer.y);
                if (handle_pointer_event(event, &drag, &pointerButtons)) {
                    *sceneDirty = true;
                } else if (!*sceneDirty && (previousCursor.x != cursor.x || previousCursor.y != cursor.y)) {
                    present_cursor_move(*sceneBuffer, fb, previousCursor, cursor);
                }
            }
            return;
        }

        if ((message.flags & std::IPC_MESSAGE_REQUEST) == 0) {
            return;
        }

        std::services::graphics_compositor::HelloReply reply = {};
        if (!build_hello_reply(message, &reply)) {
            write_str("[graphics.compositor] invalid request payload\n");
            return;
        }

        if (std::queue_reply(queue, message.id, &reply, sizeof(reply)) == fail) {
            write_str("[graphics.compositor] queue_reply failed\n");
        }
    };

    for (;;) {
        bool sceneDirty = pump_surface_updates(gSurfaceCache);

        if (!sceneDirty) {
            std::IPCMessage message = {};
            if (std::queue_receive(queue, &message, true) != fail) {
                handle_message(message, &sceneDirty);
            }
        }

        for (;;) {
            std::IPCMessage message = {};
            if (std::queue_receive(queue, &message, false) == fail) {
                break;
            }
            handle_message(message, &sceneDirty);
        }

        if (pump_surface_updates(gSurfaceCache)) {
            sceneDirty = true;
        }

        if (sceneDirty) {
            sceneBuffer = &acquire_render_buffer(&buffers);
            redraw_scene(*sceneBuffer, gSurfaceCache);
            present_to_framebuffer(*sceneBuffer, fb);
            present_cursor_move(*sceneBuffer, fb, cursor, cursor);
        }
    }

    std::close(queue);
    return 1;
}
