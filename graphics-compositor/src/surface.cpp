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

    entry->valid = true;
    entry->id = info.id;
    entry->info = info;
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
// still use their addresses.
void forget_unshown_surfaces(SurfaceCacheEntry* cache, const std::WindowInfo* windows, std::uint64_t windowCount) {
    for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
        if (!cache[i].valid) {
            continue;
        }
        bool shown = false;
        for (std::uint64_t w = 0; w < windowCount && !shown; ++w) {
            shown = windows[w].surfaceID == cache[i].id;
        }
        if (!shown) {
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

int window_control_slot_from_left(int control) {
    if (control == 0) {
        return 0;
    }
    if (control == 2) {
        return 1;
    }
    return 2;
}

Rect window_control_rect(const std::WindowInfo& window, int control) {
    const int size = kButtonSize;
    const int x = window.x + kButtonMarginLeft + (window_control_slot_from_left(control) * (size + kButtonGap));
    const int y = window.y + ((kTitleBarHeight - size) / 2);
    return { x, y, size, size };
}

void draw_window_control_icon_fallback(ui::Canvas& canvas, const Rect& rect, int control, bool maximized) {
    const ui::Color iconColor = ui::rgb(kTitleText);
    const int centerX = rect.x + (rect.width / 2);
    const int centerY = rect.y + (rect.height / 2);
    const int half = kControlIconSize / 2;
    const int thickness = 2;

    if (control == 0) {
        canvas.line({ centerX - half, centerY - half }, { centerX + half, centerY + half }, iconColor, thickness);
        canvas.line({ centerX + half, centerY - half }, { centerX - half, centerY + half }, iconColor, thickness);
        return;
    }

    if (control == 1) {
        const int left = centerX - half;
        const int top = centerY - half;
        if (maximized) {
            canvas.fill({ left + 3, top, kControlIconSize - 1, thickness }, iconColor);
            canvas.fill({ left + kControlIconSize, top, thickness, kControlIconSize - 1 }, iconColor);
            canvas.fill({ left + 3, top + kControlIconSize - 1, kControlIconSize - 1, thickness }, iconColor);
            canvas.fill({ left + 2, top + 3, thickness, kControlIconSize - 1 }, iconColor);
        } else {
            canvas.fill({ left, top, kControlIconSize + 1, thickness }, iconColor);
            canvas.fill({ left, top, thickness, kControlIconSize + 1 }, iconColor);
            canvas.fill({ left, top + kControlIconSize, kControlIconSize + 1, thickness }, iconColor);
            canvas.fill({ left + kControlIconSize, top, thickness, kControlIconSize + 1 }, iconColor);
        }
        return;
    }

    canvas.fill({ centerX - half, centerY, kControlIconSize + 1, thickness }, iconColor);
}

void draw_window_control_icon(ui::Canvas& canvas, const Rect& rect, int control,
                              bool maximized, const WindowControlAssets* controls) {
    const ui::Image* icon = nullptr;
    if (controls) {
        if (control == 0) {
            icon = &controls->close;
        } else if (control == 1) {
            icon = &controls->resize;
        } else {
            icon = &controls->minimize;
        }
    }

    if (icon && icon->valid()) {
        const int iconSize = kControlIconSize + 6;
        canvas.image(*icon, Rect { rect.x + ((rect.width - iconSize) / 2), rect.y + ((rect.height - iconSize) / 2),
                                   iconSize, iconSize });
        return;
    }

    draw_window_control_icon_fallback(canvas, rect, control, maximized);
}

void draw_window_frame(ui::Canvas& canvas, const std::WindowInfo& window, const WindowControlAssets* controls) {
    const bool focused = (window.state & std::WindowStateFocused) != 0;
    const bool maximized = (window.state & std::WindowStateMaximized) != 0;
    const int frameX = window.x;
    const int frameY = window.y;
    const int totalWidth = frame_width(window);
    const int totalHeight = frame_height(window);
    const int innerX = frameX + kBorder;
    const int innerY = frameY + kBorder;
    const int innerWidth = window.width;
    const int innerHeight = totalHeight - (kBorder * 2);
    const int innerRadius = kWindowCornerRadius > kBorder ? (kWindowCornerRadius - kBorder) : 0;

    canvas.fill_rounded({ frameX, frameY, totalWidth, totalHeight }, kWindowCornerRadius,
                        ui::rgb(focused ? kWindowBorderFocused : kWindowBorder));
    canvas.fill_rounded({ innerX, innerY, innerWidth, innerHeight }, innerRadius, ui::rgb(kWindowBackground));
    canvas.fill_top_rounded({ innerX, innerY, innerWidth, kTitleBarHeight - kBorder }, innerRadius,
                            ui::rgb(focused ? kTitleBarFocused : kTitleBar));
    canvas.fill_top_rounded({ innerX, innerY, innerWidth, 1 }, innerRadius, ui::rgb(kTitleBarHighlight));

    draw_window_control_icon(canvas, window_control_rect(window, 2), 2, maximized, controls);
    draw_window_control_icon(canvas, window_control_rect(window, 1), 1, maximized, controls);
    draw_window_control_icon(canvas, window_control_rect(window, 0), 0, maximized, controls);

    const ui::Font& font = ui::Font::ui();
    const char* title = window.title[0] ? window.title : "Window";
    canvas.text(frameX + ((totalWidth - font.width(title)) / 2), frameY + 22, title, font, ui::rgb(kTitleText));
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
