#include "common.hpp"

int gTaskbarHover = -1;

Rect taskbar_bar_rect(std::uint32_t screenWidth, std::uint32_t screenHeight) {
    return { kTaskbarMargin, static_cast<int>(screenHeight) - kTaskbarMargin - kTaskbarHeight,
             static_cast<int>(screenWidth) - 2 * kTaskbarMargin, kTaskbarHeight };
}

int work_area_bottom(std::uint32_t screenHeight) {
    return static_cast<int>(screenHeight) - kTaskbarMargin - kTaskbarHeight - kWorkAreaGap;
}

namespace {

int pinned_slot_of(const char* title) {
    for (int slot = 0; slot < kPinnedCount; ++slot) {
        if (std::strncmp(title, taskbar_slot_window_title(slot), sizeof(std::WindowInfo::title)) == 0) {
            return slot;
        }
    }
    return -1;
}

bool is_open(const std::WindowInfo& window) {
    return (window.state & std::WindowStateClosed) == 0;
}

bool is_active(const std::WindowInfo& window) {
    return (window.state & std::WindowStateFocused) != 0 && (window.state & std::WindowStateMinimized) == 0;
}

// The window a tile stands for: an unpinned tile's own, a pinned app's
// focused window or else its newest. Null when the app has none open.
const std::WindowInfo* tile_window(const TaskbarTile& tile, const std::WindowInfo* windows, std::uint64_t windowCount) {
    if (tile.slot < 0) {
        const std::WindowInfo* window = window_by_id(windows, windowCount, tile.windowId);
        return window && is_open(*window) ? window : nullptr;
    }
    const std::WindowInfo* best = nullptr;
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        const std::WindowInfo& window = windows[i];
        if (!is_open(window) || pinned_slot_of(window.title) != tile.slot) {
            continue;
        }
        if (!best || is_active(window) || (!is_active(*best) && window.id > best->id)) {
            best = &window;
        }
    }
    return best;
}

const char* tile_label(const TaskbarTile& tile, const std::WindowInfo* window) {
    if (window && window->title[0] != '\0') {
        return window->title;
    }
    return tile.slot >= 0 ? taskbar_slot_label(tile.slot) : "Window";
}

// "29 Oct 2026".
void format_date(const std::Time::DateTime& now, char* out, std::size_t size) {
    static const char* const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const char* month = now.month >= 1 && now.month <= 12 ? kMonths[now.month - 1] : "";
    char buffer[24] = {};
    std::size_t n = 0;
    if (now.day >= 10) {
        buffer[n++] = static_cast<char>('0' + now.day / 10);
    }
    buffer[n++] = static_cast<char>('0' + now.day % 10);
    buffer[n++] = ' ';
    for (std::size_t i = 0; month[i] != '\0'; ++i) {
        buffer[n++] = month[i];
    }
    buffer[n++] = ' ';
    char year[6] = {};
    int y = now.year;
    for (int d = 3; d >= 0; --d) {
        year[d] = static_cast<char>('0' + y % 10);
        y /= 10;
    }
    for (int d = 0; d < 4; ++d) {
        buffer[n++] = year[d];
    }
    buffer[n] = '\0';
    std::strncpy(out, buffer, size - 1);
    out[size - 1] = '\0';
}

// The blurred wallpaper under the bar, cut to its rounded corners.
void draw_bar_backdrop(ui::Canvas& canvas, const Rect& bar, const DesktopBackground* background) {
    if (!background || !background->barBlur || background->barBlurRect.x != bar.x ||
        background->barBlurRect.y != bar.y || background->barBlurRect.width != bar.width ||
        background->barBlurRect.height != bar.height) {
        return;
    }
    const Rect area = bar.intersect(canvas.clip());
    for (int y = area.y; y < area.bottom(); ++y) {
        int left = area.x;
        while (left < area.right() && !ui::in_rounded_rect(bar, kTaskbarRadius, left, y)) {
            ++left;
        }
        int right = area.right();
        while (right > left && !ui::in_rounded_rect(bar, kTaskbarRadius, right - 1, y)) {
            --right;
        }
        if (right > left) {
            canvas.copy(background->barBlur, bar.width, { left - bar.x, y - bar.y, right - left, 1 }, { left, y });
        }
    }
}

}  // namespace

TaskbarLayout taskbar_layout(std::uint32_t screenWidth, std::uint32_t screenHeight,
                             const std::WindowInfo* windows, std::uint64_t windowCount) {
    TaskbarLayout layout = {};
    layout.bar = taskbar_bar_rect(screenWidth, screenHeight);
    layout.start = { layout.bar.x + 8, layout.bar.y + (kTaskbarHeight - kStartSize) / 2, kStartSize, kStartSize };

    for (int slot = 0; slot < kPinnedCount; ++slot) {
        layout.tiles[layout.tileCount++] = { {}, slot, 0 };
    }
    // Windows of other apps, oldest first (window ids only grow).
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        if (!is_open(windows[i]) || pinned_slot_of(windows[i].title) >= 0) {
            continue;
        }
        if (layout.tileCount == kMaxTiles && windows[i].id > layout.tiles[kMaxTiles - 1].windowId) {
            continue;
        }
        int at = layout.tileCount < kMaxTiles ? layout.tileCount++ : kMaxTiles - 1;
        while (at > kPinnedCount && layout.tiles[at - 1].windowId > windows[i].id) {
            layout.tiles[at] = layout.tiles[at - 1];
            --at;
        }
        layout.tiles[at] = { {}, -1, windows[i].id };
    }

    const int total = layout.tileCount * kTileSize + (layout.tileCount - 1) * kTileGap;
    const int x0 = layout.bar.x + (layout.bar.width - total) / 2;
    for (int i = 0; i < layout.tileCount; ++i) {
        layout.tiles[i].rect = { x0 + i * (kTileSize + kTileGap), layout.bar.y + (kTaskbarHeight - kTileSize) / 2,
                                 kTileSize, kTileSize };
    }
    return layout;
}

TaskbarHit taskbar_hit(const TaskbarLayout& layout, int x, int y) {
    if (!layout.bar.contains(x, y)) {
        return { TaskbarHitKind::None, -1 };
    }
    if (layout.start.contains(x, y)) {
        return { TaskbarHitKind::Start, -1 };
    }
    for (int i = 0; i < layout.tileCount; ++i) {
        if (layout.tiles[i].rect.contains(x, y)) {
            return { TaskbarHitKind::Tile, i };
        }
    }
    return { TaskbarHitKind::Bar, -1 };
}

Rect taskbar_tooltip_rect(const TaskbarLayout& layout, int tile, const std::WindowInfo* windows,
                          std::uint64_t windowCount) {
    if (tile < 0 || tile >= layout.tileCount) {
        return { 0, 0, 0, 0 };
    }
    const TaskbarTile& t = layout.tiles[tile];
    const ui::Font& font = ui::theme().fontSmall();
    const int width = font.width(tile_label(t, tile_window(t, windows, windowCount))) + 20;
    int x = t.rect.x + t.rect.width / 2 - width / 2;
    if (x < layout.bar.x) {
        x = layout.bar.x;
    }
    if (x + width > layout.bar.right()) {
        x = layout.bar.right() - width;
    }
    return { x, layout.bar.y - 8 - kTooltipHeight, width, kTooltipHeight };
}

bool taskbar_click(const TaskbarLayout& layout, int tile, const std::WindowInfo* windows, std::uint64_t windowCount) {
    if (tile < 0 || tile >= layout.tileCount) {
        return false;
    }
    const TaskbarTile& t = layout.tiles[tile];
    const std::WindowInfo* window = tile_window(t, windows, windowCount);
    if (!window) {
        if (t.slot >= 0 && !launch_taskbar_slot(t.slot)) {
            write_str("[graphics.compositor] FAIL spawn taskbar slot\n");
        }
        return false;
    }
    if (is_active(*window)) {
        std::compositor_control_window(window->id, std::WindowControlAction::Minimize);
    } else {
        std::compositor_focus_window(window->id);   // also brings a minimized one back
    }
    return true;
}

void draw_taskbar(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                  const TaskbarAssets* assets, const DesktopBackground* background) {
    const ui::Theme& t = ui::theme();
    const TaskbarLayout layout = taskbar_layout(static_cast<std::uint32_t>(canvas.width()),
                                                static_cast<std::uint32_t>(canvas.height()), windows, windowCount);
    const Rect& bar = layout.bar;

    draw_bar_backdrop(canvas, bar, background);
    canvas.fill_rounded(bar, kTaskbarRadius, ui::rgba(0x0e131b, 219));
    canvas.frame(bar, t.border, 1, kTaskbarRadius);

    // Start button and wordmark.
    canvas.fill_rounded(layout.start, kStartRadius, t.accent);
    const ui::Font& startFont = assets && assets->startFont.valid() ? assets->startFont : t.fontMedium();
    canvas.text(layout.start, "IO", startFont, t.accentText, ui::AlignCenter | ui::AlignMiddle);
    canvas.text({ layout.start.right() + 10, bar.y, 120, bar.height }, "InstantOS", t.fontMedium(), t.text,
                ui::AlignLeft | ui::AlignMiddle);

    // App tiles: a running app has a dot, the focused one a light tile and
    // an accent bar.
    for (int i = 0; i < layout.tileCount; ++i) {
        const TaskbarTile& tile = layout.tiles[i];
        const std::WindowInfo* window = tile_window(tile, windows, windowCount);
        const bool active = window && is_active(*window);
        if (active) {
            canvas.fill_rounded(tile.rect, kTileRadius, ui::rgba(0xffffff, 26));
        } else if (i == gTaskbarHover) {
            canvas.fill_rounded(tile.rect, kTileRadius, ui::rgba(0xffffff, 15));
        }
        const int icon = tile.slot >= 0 ? tile.slot : title_icon_index(window ? window->title : "");
        const ui::Image* image = assets ? &assets->apps[icon][window ? 0 : 1] : nullptr;
        if (image && image->valid()) {
            canvas.image(*image, ui::Point { tile.rect.x + (kTileSize - kTileIconSize) / 2,
                                             tile.rect.y + (kTileSize - kTileIconSize) / 2 - 1 });
        }
        if (active) {
            canvas.fill_rounded({ tile.rect.x + (kTileSize - 16) / 2, tile.rect.bottom() - 4, 16, 3 }, 2, t.accentLight);
        } else if (window) {
            canvas.fill_rounded({ tile.rect.x + (kTileSize - 6) / 2, tile.rect.bottom() - 4, 6, 3 }, 2, t.textDim);
        }
    }

    // Tray: network, sound, then the clock.
    const int trayY = bar.y + (kTaskbarHeight - kTrayIconSize) / 2;
    if (assets) {
        const ui::Image& network = gNetworkOnline ? assets->wifi : assets->wifiOff;
        if (network.valid()) {
            canvas.image(network, ui::Point { bar.right() - 168, trayY });
        }
        if (assets->volumeOff.valid()) {
            canvas.image(assets->volumeOff, ui::Point { bar.right() - 138, trayY });
        }
    }
    canvas.fill({ bar.right() - 108, bar.y + 14, 1, kTaskbarHeight - 28 }, t.border);
    const std::Time::DateTime now = std::Time::now();
    const std::Time::TimeString timeText = std::Time::time_string(now);
    char dateText[24] = {};
    format_date(now, dateText, sizeof(dateText));
    canvas.text({ bar.right() - 104, bar.y + 7, 88, 20 }, timeText.value, t.fontMedium(), t.text,
                ui::AlignRight | ui::AlignMiddle);
    canvas.text({ bar.right() - 104, bar.y + 26, 88, 18 }, dateText, t.fontSmall(), t.textDim,
                ui::AlignRight | ui::AlignMiddle);

    // The hovered tile's tooltip.
    if (gTaskbarHover >= 0 && gTaskbarHover < layout.tileCount) {
        const TaskbarTile& tile = layout.tiles[gTaskbarHover];
        const Rect tip = taskbar_tooltip_rect(layout, gTaskbarHover, windows, windowCount);
        canvas.fill_rounded(tip, 8, t.tooltip);
        canvas.frame(tip, t.border, 1, 8);
        canvas.text(tip, tile_label(tile, tile_window(tile, windows, windowCount)), t.fontSmall(), t.text,
                    ui::AlignCenter | ui::AlignMiddle);
    }
}

// Everything the canvas's clip covers: wallpaper, the windows that reach
// into it (bottom to top), the taskbar.
static void draw_scene(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                       SurfaceCacheEntry* cache, const DesktopBackground* background,
                       const TaskbarAssets* taskbar, const WindowControlAssets* controls) {
    const std::uint64_t redrawStart = std::gettime();
    std::uint64_t phaseStart = std::gettime();
    draw_background(canvas, background);
    add_elapsed(phaseStart, &gTiming.backgroundMs);

    const Rect clip = canvas.clip();
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        if (!is_window_visible(windows[i])) {
            continue;
        }

        const Rect frame = window_damage_rect(windows[i]);
        if (frame.intersect(clip).empty()) {
            continue;
        }
        phaseStart = std::gettime();
        draw_window_frame(canvas, windows[i], controls);
        add_elapsed(phaseStart, &gTiming.windowFrameMs);
        phaseStart = std::gettime();
        blit_window_surface(canvas, windows[i], find_surface_cache(cache, windows[i].surfaceID));
        add_elapsed(phaseStart, &gTiming.surfaceBlitMs);
    }

    draw_taskbar(canvas, windows, windowCount, taskbar, background);
    add_elapsed(redrawStart, &gTiming.sceneRedrawMs);
}

void redraw_scene(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                  SurfaceCacheEntry* cache, const DesktopBackground* background,
                  const TaskbarAssets* taskbar, const WindowControlAssets* controls) {
    draw_scene(canvas, windows, windowCount, cache, background, taskbar, controls);
}

void redraw_scene_rect(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                       SurfaceCacheEntry* cache, const DesktopBackground* background,
                       const Rect& dirty, const TaskbarAssets* taskbar, const WindowControlAssets* controls) {
    ui::Canvas clipped = canvas.clipped(dirty);
    if (clipped.clip().empty()) {
        return;
    }
    draw_scene(clipped, windows, windowCount, cache, background, taskbar, controls);
}
