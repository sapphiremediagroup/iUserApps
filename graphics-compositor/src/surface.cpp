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
    const Rect merged = union_rect(oldDirty, newDirty);
    entry->dirty = !rect_is_empty(merged);
    entry->dirtyX = static_cast<std::uint32_t>(merged.x);
    entry->dirtyY = static_cast<std::uint32_t>(merged.y);
    entry->dirtyWidth = static_cast<std::uint32_t>(merged.width);
    entry->dirtyHeight = static_cast<std::uint32_t>(merged.height);
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

void draw_line_rect(const RenderBuffer& buffer, int x0, int y0, int x1, int y1, int thickness, const RGBATexel& color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        fill_rect(buffer, x0 - (thickness / 2), y0 - (thickness / 2), thickness, thickness, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twiceError = error * 2;
        if (twiceError >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twiceError <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void draw_window_control_icon_fallback(const RenderBuffer& buffer, const Rect& rect, int control, bool maximized) {
    const RGBATexel iconColor = rgba_from_rgb(kTitleText);
    const int centerX = rect.x + (rect.width / 2);
    const int centerY = rect.y + (rect.height / 2);
    const int half = kControlIconSize / 2;
    const int thickness = 2;

    if (control == 0) {
        draw_line_rect(buffer, centerX - half, centerY - half, centerX + half, centerY + half, thickness, iconColor);
        draw_line_rect(buffer, centerX + half, centerY - half, centerX - half, centerY + half, thickness, iconColor);
        return;
    }

    if (control == 1) {
        const int left = centerX - half;
        const int top = centerY - half;
        if (maximized) {
            fill_rect(buffer, left + 3, top, kControlIconSize - 1, thickness, iconColor);
            fill_rect(buffer, left + kControlIconSize, top, thickness, kControlIconSize - 1, iconColor);
            fill_rect(buffer, left + 3, top + kControlIconSize - 1, kControlIconSize - 1, thickness, iconColor);
            fill_rect(buffer, left + 2, top + 3, thickness, kControlIconSize - 1, iconColor);
        } else {
            fill_rect(buffer, left, top, kControlIconSize + 1, thickness, iconColor);
            fill_rect(buffer, left, top, thickness, kControlIconSize + 1, iconColor);
            fill_rect(buffer, left, top + kControlIconSize, kControlIconSize + 1, thickness, iconColor);
            fill_rect(buffer, left + kControlIconSize, top, thickness, kControlIconSize + 1, iconColor);
        }
        return;
    }

    fill_rect(buffer, centerX - half, centerY, kControlIconSize + 1, thickness, iconColor);
}

void draw_window_control_icon(const RenderBuffer& buffer, const Rect& rect, int control,
                              bool maximized, const WindowControlAssets* controls) {
    const ImageAsset* icon = nullptr;
    if (controls) {
        if (control == 0) {
            icon = &controls->close;
        } else if (control == 1) {
            icon = &controls->resize;
        } else {
            icon = &controls->minimize;
        }
    }

    if (icon && icon->ready) {
        const int iconSize = kControlIconSize + 6;
        draw_image_scaled(buffer,
                          *icon,
                          rect.x + ((rect.width - iconSize) / 2),
                          rect.y + ((rect.height - iconSize) / 2),
                          iconSize,
                          iconSize);
        return;
    }

    draw_window_control_icon_fallback(buffer, rect, control, maximized);
}

void draw_window_frame(const RenderBuffer& buffer, const std::WindowInfo& window, const WindowControlAssets* controls) {
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

    fill_rounded_rect(buffer, frameX, frameY, totalWidth, totalHeight, kWindowCornerRadius, rgba_from_rgb(focused ? kWindowBorderFocused : kWindowBorder));
    fill_rounded_rect(buffer, innerX, innerY, innerWidth, innerHeight, innerRadius, rgba_from_rgb(kWindowBackground));
    fill_top_rounded_rect(buffer, innerX, innerY, innerWidth, kTitleBarHeight - kBorder, innerRadius, rgba_from_rgb(focused ? kTitleBarFocused : kTitleBar));
    fill_top_rounded_rect(buffer, innerX, innerY, innerWidth, 1, innerRadius, rgba_from_rgb(kTitleBarHighlight));

    draw_window_control_icon(buffer, window_control_rect(window, 2), 2, maximized, controls);
    draw_window_control_icon(buffer, window_control_rect(window, 1), 1, maximized, controls);
    draw_window_control_icon(buffer, window_control_rect(window, 0), 0, maximized, controls);

    const char* title = window.title[0] ? window.title : "Window";
    const int titleWidth = text_width(gUIFont, title);
    draw_text(buffer, gUIFont, frameX + ((totalWidth - titleWidth) / 2), frameY + 22, title, rgba_from_rgb(kTitleText));
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
    const int innerRadius = kWindowCornerRadius > kBorder ? (kWindowCornerRadius - kBorder) : 0;
    const int clipBottomBandStart = window.y + frame_height(window) - kBorder - innerRadius;
    const std::uint32_t sourcePitch = surfaceEntry->info.pitch >= 4
        ? surfaceEntry->info.pitch / 4
        : surfaceEntry->info.width;
    const std::uint32_t copyWidth = surfaceEntry->info.width < static_cast<std::uint32_t>(window.width)
        ? surfaceEntry->info.width
        : static_cast<std::uint32_t>(window.width);
    const std::uint32_t copyHeight = surfaceEntry->info.height < static_cast<std::uint32_t>(window.height)
        ? surfaceEntry->info.height
        : static_cast<std::uint32_t>(window.height);

    std::uint32_t sourceStartY = 0;
    std::uint32_t visibleHeight = copyHeight;
    int drawStartY = destY;
    if (drawStartY < 0) {
        sourceStartY = static_cast<std::uint32_t>(-drawStartY);
        if (sourceStartY >= visibleHeight) {
            return;
        }
        visibleHeight -= sourceStartY;
        drawStartY = 0;
    }
    if (drawStartY + static_cast<int>(visibleHeight) > static_cast<int>(buffer.height)) {
        visibleHeight = static_cast<std::uint32_t>(static_cast<int>(buffer.height) - drawStartY);
    }
    if (visibleHeight == 0) {
        return;
    }

    std::uint32_t sourceStartX = 0;
    std::uint32_t visibleWidth = copyWidth;
    int drawStartX = destX;
    if (drawStartX < 0) {
        sourceStartX = static_cast<std::uint32_t>(-drawStartX);
        if (sourceStartX >= visibleWidth) {
            return;
        }
        visibleWidth -= sourceStartX;
        drawStartX = 0;
    }
    if (drawStartX + static_cast<int>(visibleWidth) > static_cast<int>(buffer.width)) {
        visibleWidth = static_cast<std::uint32_t>(static_cast<int>(buffer.width) - drawStartX);
    }
    if (visibleWidth == 0) {
        return;
    }

    if (gDrawClipEnabled) {
        const Rect visible = { drawStartX, drawStartY, static_cast<int>(visibleWidth), static_cast<int>(visibleHeight) };
        const Rect clipped = intersect_rect(visible, gDrawClip);
        if (rect_is_empty(clipped)) {
            return;
        }
        sourceStartX += static_cast<std::uint32_t>(clipped.x - drawStartX);
        sourceStartY += static_cast<std::uint32_t>(clipped.y - drawStartY);
        drawStartX = clipped.x;
        drawStartY = clipped.y;
        visibleWidth = static_cast<std::uint32_t>(clipped.width);
        visibleHeight = static_cast<std::uint32_t>(clipped.height);
    }

    for (std::uint32_t y = 0; y < visibleHeight; ++y) {
        const std::uint32_t sourceY = sourceStartY + y;
        const int drawY = drawStartY + static_cast<int>(y);
        if (drawY < 0 || drawY >= static_cast<int>(buffer.height)) {
            continue;
        }

        if (innerRadius <= 0 || drawY < clipBottomBandStart) {
            copy_rgb_row_to_texels(
                &buffer.pixels[drawY * buffer.pitch + static_cast<std::uint32_t>(drawStartX)],
                &src[sourceY * sourcePitch + sourceStartX],
                visibleWidth
            );
            continue;
        }

        for (std::uint32_t x = 0; x < visibleWidth; ++x) {
            const std::uint32_t sourceX = sourceStartX + x;
            const int drawX = drawStartX + static_cast<int>(x);
            if (drawX < 0 || drawX >= static_cast<int>(buffer.width)) {
                continue;
            }
            if (innerRadius > 0 &&
                drawY >= clipBottomBandStart &&
                !point_in_rounded_rect(drawX, drawY, window.x + kBorder, window.y + kBorder,
                                       window.width, frame_height(window) - (kBorder * 2), innerRadius)) {
                continue;
            }

            buffer.pixels[drawY * buffer.pitch + drawX] = rgb_to_texel_word(src[sourceY * sourcePitch + sourceX]);
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
        dirty = union_rect(dirty, windowDirty);
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
