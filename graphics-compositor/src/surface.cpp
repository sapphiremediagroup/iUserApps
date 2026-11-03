#include "common.hpp"

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

    if (!entry->valid || entry->id != info.id) {
        entry->generation = 0;
    }
    entry->valid = true;
    entry->id = info.id;
    entry->info = info;
    entry->unshownLists = 0;   // just committed: about to be shown
    ++entry->generation;   // the GL renderer uploads a shared-memory surface again
    const Rect oldDirty = entry->dirty
        ? Rect { static_cast<int>(entry->dirtyX), static_cast<int>(entry->dirtyY),
                 static_cast<int>(entry->dirtyWidth), static_cast<int>(entry->dirtyHeight) }
        : Rect { 0, 0, 0, 0 };
    const Rect newDirty = {
        static_cast<int>(info.dirtyX),
        static_cast<int>(info.dirtyY),
        static_cast<int>(info.dirtyWidth),
        static_cast<int>(info.dirtyHeight)
    };
    const Rect merged = oldDirty.unite(newDirty);
    entry->dirty = !merged.empty();
    entry->dirtyX = static_cast<std::uint32_t>(merged.x);
    entry->dirtyY = static_cast<std::uint32_t>(merged.y);
    entry->dirtyWidth = static_cast<std::uint32_t>(merged.width);
    entry->dirtyHeight = static_cast<std::uint32_t>(merged.height);
}

// Drops the surfaces no window shows any more (a window that was resized got
// a new one, or closed). The kernel unmaps a dead surface from us when we
// list the windows, so this runs on each new list and nothing below may
// still use their addresses (only a shown surface's entry is ever read).
void forget_unshown_surfaces(SurfaceCacheEntry* cache, const std::WindowInfo* windows, std::uint64_t windowCount) {
    for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
        if (!cache[i].valid) {
            continue;
        }
        bool shown = false;
        for (std::uint64_t w = 0; w < windowCount && !shown; ++w) {
            shown = windows[w].surfaceID == cache[i].id;
        }
        // A surface committed but not attached yet (a program commits, then
        // attaches) must survive the list in between, or the window shows no
        // pixels until that surface is committed again: a GPU program's
        // window blinked. Only surfaces a window shows are ever read, and
        // those are alive, so a dead one's entry may stay a few lists.
        if (shown) {
            cache[i].unshownLists = 0;
        } else if (++cache[i].unshownLists > kSurfaceForgetLists) {
            cache[i] = {};
        }
    }
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

WindowHover gWindowHover = {};

// Close at the right edge, then maximize, then minimize.
Rect window_control_rect(const std::WindowInfo& window, int control) {
    const int slotFromRight = control == kControlClose ? 0 : control == kControlMinimize ? 2 : 1;
    const int x = window.x + frame_width(window) - kBorder - (slotFromRight + 1) * kControlWidth;
    return { x, window.y + kBorder, kControlWidth, kTitleBarHeight - kBorder };
}

int title_icon_index(const char* title) {
    static const char* const kTitles[kTitleIconCount - 1] = { "Terminal", "File Browser", "Background Switcher",
                                                               "Cube", "NetSurf" };
    for (int i = 0; i < kTitleIconCount - 1; ++i) {
        if (std::strcmp(title, kTitles[i]) == 0) {
            return i;
        }
    }
    return kTitleIconCount - 1;
}

namespace {

// A soft shadow under the frame: black fading with the distance from the
// frame's rounded shape moved down by kShadowOffsetY. The alpha depends only
// on how far a pixel is past the corner circle's centre in x and in y, so it
// comes from a table made once per focus state. Only the ring outside the
// frame is drawn; the frame covers the rest.
constexpr int kShadowReach = kShadowBlur + kWindowCornerRadius + 1;

struct ShadowTable {
    bool ready;
    std::uint8_t alpha[kShadowReach][kShadowReach];
};

const ShadowTable& shadow_table(bool focused) {
    static ShadowTable tables[2] = {};
    ShadowTable& table = tables[focused ? 1 : 0];
    if (!table.ready) {
        const float maxAlpha = focused ? 120.0f : 70.0f;
        for (int a = 0; a < kShadowReach; ++a) {
            for (int b = 0; b < kShadowReach; ++b) {
                const float distance = sqrtf(static_cast<float>(a * a + b * b)) - kWindowCornerRadius;
                const float t = distance <= 0.0f ? 1.0f : distance >= kShadowBlur ? 0.0f : 1.0f - distance / kShadowBlur;
                table.alpha[a][b] = static_cast<std::uint8_t>(maxAlpha * t * t);
            }
        }
        table.ready = true;
    }
    return table;
}

void draw_window_shadow(ui::Canvas& canvas, const Rect& frame, bool focused) {
    const Rect shape = frame.offset(0, kShadowOffsetY);
    const Rect clip = canvas.clip();
    if (Rect { shape.x - kShadowBlur, shape.y - kShadowBlur, shape.width + 2 * kShadowBlur,
               shape.height + 2 * kShadowBlur }.intersect(clip).empty()) {
        return;
    }
    const ShadowTable& table = shadow_table(focused);
    // The corner circles' centres.
    const int left = shape.x + kWindowCornerRadius;
    const int right = shape.right() - 1 - kWindowCornerRadius;
    const int top = shape.y + kWindowCornerRadius;
    const int bottom = shape.bottom() - 1 - kWindowCornerRadius;
    auto shade = [](std::uint8_t alpha) { return ui::rgba(0x000000, alpha); };

    // Along the straight edges the alpha is the same down a column (sides)
    // or along a row (top, bottom), so those are 1 px spans; the parts the
    // frame covers are left out.
    for (int d = 1; d < kShadowReach; ++d) {
        const std::uint8_t alpha = table.alpha[d][0];
        if (alpha == 0) {
            break;
        }
        // Left and right columns, between the corner rows, outside the frame.
        canvas.fill({ left - d, top, 1, bottom - top + 1 }, shade(left - d < frame.x ? alpha : 0));
        canvas.fill({ right + d, top, 1, bottom - top + 1 }, shade(right + d >= frame.right() ? alpha : 0));
        // Top and bottom rows, between the corner columns.
        if (top - d < frame.y) {
            canvas.fill({ left, top - d, right - left + 1, 1 }, shade(alpha));
        }
        if (bottom + d >= frame.bottom()) {
            canvas.fill({ left, bottom + d, right - left + 1, 1 }, shade(alpha));
        }
    }
    // The four corners, pixel by pixel.
    const ui::Color black = ui::rgb(0x000000);
    const int cornerX[2] = { left, right };
    const int cornerY[2] = { top, bottom };
    for (int cy = 0; cy < 2; ++cy) {
        for (int cx = 0; cx < 2; ++cx) {
            const int stepX = cx == 0 ? -1 : 1;
            const int stepY = cy == 0 ? -1 : 1;
            for (int dy = 1; dy < kShadowReach; ++dy) {
                const int y = cornerY[cy] + stepY * dy;
                if (y < clip.y || y >= clip.bottom()) {
                    continue;
                }
                for (int dx = 1; dx < kShadowReach; ++dx) {
                    const int x = cornerX[cx] + stepX * dx;
                    if (x < clip.x || x >= clip.right()) {
                        continue;
                    }
                    if (x >= frame.x && x < frame.right() && y >= frame.y && y < frame.bottom() &&
                        ui::in_rounded_rect(frame, kWindowCornerRadius, x, y)) {
                        continue;
                    }
                    const std::uint8_t alpha = table.alpha[dx][dy];
                    if (alpha != 0) {
                        canvas.blend_pixel(x, y, black, alpha);
                    }
                }
            }
        }
    }
}

// The close button's red, cut to the frame's rounded top right corner.
void fill_close_hover(ui::Canvas& canvas, const Rect& button, const Rect& inner, int radius, ui::Color color) {
    const Rect area = button.intersect(canvas.clip());
    for (int y = area.y; y < area.bottom(); ++y) {
        int right = area.right();
        while (right > area.x && !ui::in_rounded_rect(inner, radius, right - 1, y)) {
            --right;
        }
        if (right > area.x) {
            canvas.fill({ area.x, y, right - area.x, 1 }, color);
        }
    }
}

}  // namespace

void draw_window_frame(ui::Canvas& canvas, const std::WindowInfo& window, const WindowControlAssets* controls) {
    const ui::Theme& t = ui::theme();
    const bool focused = (window.state & std::WindowStateFocused) != 0;
    const bool maximized = (window.state & std::WindowStateMaximized) != 0;
    const Rect frame = { window.x, window.y, frame_width(window), frame_height(window) };
    const Rect inner = frame.inset(kBorder);
    const int innerRadius = kWindowCornerRadius - kBorder;

    draw_window_shadow(canvas, frame, focused);
    canvas.fill_rounded(frame, kWindowCornerRadius, t.window);
    canvas.frame(frame, ui::rgba(0xffffff, focused ? 26 : 15), 1, kWindowCornerRadius);
    (void)maximized;
    (void)inner;
    (void)innerRadius;
    draw_window_title(canvas, window, controls);
}

// What the title bar shows over the window colour: the app's icon and name,
// the controls (with the hovered one's highlight). The GL renderer draws the
// rest of the frame itself and this into a texture.
void draw_window_title(ui::Canvas& canvas, const std::WindowInfo& window, const WindowControlAssets* controls) {
    const ui::Theme& t = ui::theme();
    const bool focused = (window.state & std::WindowStateFocused) != 0;
    const bool maximized = (window.state & std::WindowStateMaximized) != 0;
    const Rect frame = { window.x, window.y, frame_width(window), frame_height(window) };
    const Rect inner = frame.inset(kBorder);
    const int innerRadius = kWindowCornerRadius - kBorder;

    // The app's icon and name.
    const int tint = focused ? 0 : 1;
    const int icon = title_icon_index(window.title);
    if (controls && controls->apps[icon][tint].valid()) {
        canvas.image(controls->apps[icon][tint],
                     ui::Point { frame.x + kTitleIconX, frame.y + (kTitleBarHeight - kTitleIconSize) / 2 });
    }
    const char* title = window.title[0] ? window.title : "Window";
    const int controlsLeft = window_control_rect(window, kControlMinimize).x;
    canvas.text({ frame.x + kTitleTextX, frame.y, controlsLeft - frame.x - kTitleTextX - 8, kTitleBarHeight }, title,
                t.fontMedium(), focused ? t.text : t.textUnfocused, ui::AlignLeft | ui::AlignMiddle);

    // Minimize, maximize (restore when maximized), close.
    for (int control = kControlClose; control <= kControlMinimize; ++control) {
        const Rect button = window_control_rect(window, control);
        const bool hovered = gWindowHover.windowId == window.id && gWindowHover.control == control;
        int glyphTint = tint;
        if (hovered && control == kControlClose) {
            fill_close_hover(canvas, button, inner, innerRadius, t.danger);
            glyphTint = 2;
        } else if (hovered) {
            canvas.fill(button, t.faceHover);
            glyphTint = 0;
        }
        const int glyph = control == kControlMaximize && maximized ? kControlRestore : control;
        if (controls && controls->glyphs[glyph][glyphTint].valid()) {
            canvas.image(controls->glyphs[glyph][glyphTint],
                         ui::Point { button.x + (button.width - kControlGlyphSize) / 2,
                                     button.y + (button.height - kControlGlyphSize) / 2 });
        }
    }
}

// The window's surface into its content area, opaque. The rows level with
// the frame's rounded bottom corners are cut to the part inside them.
void blit_window_surface(ui::Canvas& canvas, const std::WindowInfo& window, const SurfaceCacheEntry* surfaceEntry) {
    if (!surfaceEntry || !surfaceEntry->valid) {
        return;
    }

    const auto* src = reinterpret_cast<const std::uint32_t*>(surfaceEntry->info.address);
    if (!src) {
        return;
    }

    const int destX = window.x + kBorder;
    const int destY = window.y + kTitleBarHeight;
    const int innerRadius = kWindowCornerRadius > kBorder ? (kWindowCornerRadius - kBorder) : 0;
    const Rect inner = { window.x + kBorder, window.y + kBorder, window.width, frame_height(window) - (kBorder * 2) };
    const int bandStart = window.y + frame_height(window) - kBorder - innerRadius;
    const int sourcePitch = static_cast<int>(surfaceEntry->info.pitch >= 4
        ? surfaceEntry->info.pitch / 4
        : surfaceEntry->info.width);
    const int copyWidth = static_cast<int>(surfaceEntry->info.width) < window.width
        ? static_cast<int>(surfaceEntry->info.width) : window.width;
    const int copyHeight = static_cast<int>(surfaceEntry->info.height) < window.height
        ? static_cast<int>(surfaceEntry->info.height) : window.height;
    if (copyWidth <= 0 || copyHeight <= 0) {
        return;
    }

    const int bandRows = bandStart - destY;
    if (innerRadius <= 0 || bandRows >= copyHeight) {
        canvas.copy(src, sourcePitch, { 0, 0, copyWidth, copyHeight }, { destX, destY }, true);
        return;
    }
    if (bandRows > 0) {
        canvas.copy(src, sourcePitch, { 0, 0, copyWidth, bandRows }, { destX, destY }, true);
    }
    const int firstBandRow = bandRows > 0 ? bandRows : 0;
    for (int row = firstBandRow; row < copyHeight; ++row) {
        const int y = destY + row;
        int left = 0;
        while (left < copyWidth && !ui::in_rounded_rect(inner, innerRadius, destX + left, y)) {
            ++left;
        }
        int right = copyWidth;
        while (right > left && !ui::in_rounded_rect(inner, innerRadius, destX + right - 1, y)) {
            --right;
        }
        if (right > left) {
            canvas.copy(src, sourcePitch, { left, row, right - left, 1 }, { destX + left, y }, true);
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

Rect consume_surface_dirty_rect(const std::WindowInfo* windows, std::uint64_t windowCount,
                                SurfaceCacheEntry* cache, std::uint32_t width, std::uint32_t height) {
    Rect dirty = { 0, 0, 0, 0 };

    for (std::uint64_t i = 0; i < windowCount; ++i) {
        const std::WindowInfo& window = windows[i];
        if (!is_window_visible(window)) {
            continue;
        }

        SurfaceCacheEntry* entry = find_surface_cache(cache, window.surfaceID);
        if (!entry || !entry->dirty) {
            continue;
        }

        const std::uint32_t dirtyRight = entry->dirtyX + entry->dirtyWidth;
        const std::uint32_t dirtyBottom = entry->dirtyY + entry->dirtyHeight;
        const std::uint32_t surfaceRight = entry->info.width < static_cast<std::uint32_t>(window.width)
            ? entry->info.width
            : static_cast<std::uint32_t>(window.width);
        const std::uint32_t surfaceBottom = entry->info.height < static_cast<std::uint32_t>(window.height)
            ? entry->info.height
            : static_cast<std::uint32_t>(window.height);
        if (entry->dirtyWidth == 0 || entry->dirtyHeight == 0 ||
            entry->dirtyX >= surfaceRight || entry->dirtyY >= surfaceBottom) {
            continue;
        }

        const std::uint32_t clippedRight = dirtyRight < surfaceRight ? dirtyRight : surfaceRight;
        const std::uint32_t clippedBottom = dirtyBottom < surfaceBottom ? dirtyBottom : surfaceBottom;
        const Rect windowDirty = {
            window.x + kBorder + static_cast<int>(entry->dirtyX),
            window.y + kTitleBarHeight + static_cast<int>(entry->dirtyY),
            static_cast<int>(clippedRight - entry->dirtyX),
            static_cast<int>(clippedBottom - entry->dirtyY)
        };
        dirty = dirty.unite(windowDirty);
    }

    for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
        cache[i].dirty = false;
        cache[i].dirtyX = 0;
        cache[i].dirtyY = 0;
        cache[i].dirtyWidth = 0;
        cache[i].dirtyHeight = 0;
    }

    return clamp_rect(dirty, width, height);
}

void clear_surface_dirty_flags(SurfaceCacheEntry* cache) {
    for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
        cache[i].dirty = false;
        cache[i].dirtyX = 0;
        cache[i].dirtyY = 0;
        cache[i].dirtyWidth = 0;
        cache[i].dirtyHeight = 0;
    }
}
