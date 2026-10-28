#include "common.hpp"

// The taskbar's three pills: 0 left (brand), 1 centre (at its narrowest),
// 2 right (status and clock).
Rect taskbar_rect_at(std::uint32_t screenWidth, std::uint32_t screenHeight, int index) {
    const int width = static_cast<int>(screenWidth);
    const int y = static_cast<int>(screenHeight) - kTaskbarBottomMargin - kTaskbarHeight;

    if (index == 0) {
        return { kTaskbarMarginX, y, kTaskbarWidth, kTaskbarHeight };
    }
    if (index == 1) {
        return { (width - kTaskbarCenterWidth) / 2, y, kTaskbarCenterWidth, kTaskbarHeight };
    }
    return { width - kTaskbarMarginX - kTaskbarWidth, y, kTaskbarWidth, kTaskbarHeight };
}

TaskbarLayout taskbar_layout(std::uint32_t screenWidth, std::uint32_t screenHeight,
                             const std::WindowInfo* windows, std::uint64_t windowCount) {
    TaskbarLayout layout = {};

    // The open windows, oldest first (window ids only grow).
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        if ((windows[i].state & std::WindowStateClosed) != 0) {
            continue;
        }
        int at = layout.windowCount;
        if (at == kTaskbarMaxWindowButtons) {
            if (windows[i].id > layout.windowIds[at - 1]) {
                continue;
            }
            --at;
        } else {
            ++layout.windowCount;
        }
        while (at > 0 && layout.windowIds[at - 1] > windows[i].id) {
            layout.windowIds[at] = layout.windowIds[at - 1];
            --at;
        }
        layout.windowIds[at] = windows[i].id;
    }

    layout.center = taskbar_rect_at(screenWidth, screenHeight, 1);
    layout.launcherWidth = kTaskbarLauncherWidth;
    if (layout.windowCount == 0) {
        layout.windowsX = layout.center.x + layout.center.width;
        return layout;
    }

    const int room = static_cast<int>(screenWidth) - 2 * (kTaskbarMarginX + kTaskbarWidth + kTaskbarGap);
    const int fixed = kTaskbarDividerWidth + kTaskbarWindowsEndPadding;
    int buttons = layout.windowCount;
    int width = (room - kTaskbarCenterSlotCount * layout.launcherWidth - fixed) / buttons;
    if (width < kTaskbarWindowButtonMinWidth) {
        layout.launcherWidth = kTaskbarLauncherMinWidth;
        width = (room - kTaskbarCenterSlotCount * layout.launcherWidth - fixed) / buttons;
    }
    if (width < kTaskbarWindowButtonMinWidth) {
        // No room for them all: the newest go unlisted.
        width = kTaskbarWindowButtonMinWidth;
        buttons = (room - kTaskbarCenterSlotCount * layout.launcherWidth - fixed) / width;
        if (buttons < 0) {
            buttons = 0;
        }
        layout.windowCount = buttons;
    }
    if (width > kTaskbarWindowButtonWidth) {
        width = kTaskbarWindowButtonWidth;
    }

    layout.windowWidth = width;
    layout.center.width = kTaskbarCenterSlotCount * layout.launcherWidth +
        (buttons > 0 ? fixed + buttons * width : 0);
    layout.center.x = (static_cast<int>(screenWidth) - layout.center.width) / 2;
    layout.windowsX = layout.center.x + kTaskbarCenterSlotCount * layout.launcherWidth + kTaskbarDividerWidth;
    return layout;
}

TaskbarHit taskbar_hit(const TaskbarLayout& layout, std::uint32_t screenWidth, std::uint32_t screenHeight, int x, int y) {
    TaskbarHit hit = { TaskbarHitKind::None, -1, 0 };
    const Rect& center = layout.center;
    if (center.contains(x, y)) {
        hit.kind = TaskbarHitKind::Pill;
        const int launchersEnd = center.x + kTaskbarCenterSlotCount * layout.launcherWidth;
        if (x < launchersEnd) {
            hit.kind = TaskbarHitKind::Launcher;
            hit.slot = (x - center.x) / layout.launcherWidth;
        } else if (x >= layout.windowsX && layout.windowWidth > 0) {
            const int index = (x - layout.windowsX) / layout.windowWidth;
            if (index < layout.windowCount) {
                hit.kind = TaskbarHitKind::Window;
                hit.windowId = layout.windowIds[index];
            }
        }
        return hit;
    }

    for (int index = 0; index < 3; index += 2) {
        if (taskbar_rect_at(screenWidth, screenHeight, index).contains(x, y)) {
            hit.kind = TaskbarHitKind::Pill;
        }
    }
    return hit;
}

static void draw_window_buttons(ui::Canvas& canvas, const TaskbarLayout& layout,
                                const std::WindowInfo* windows, std::uint64_t windowCount) {
    const ui::Font& font = ui::Font::ui();
    const Rect& center = layout.center;
    canvas.fill({ layout.windowsX - (kTaskbarDividerWidth / 2) - 1, center.y + 18, 1, center.height - 36 },
                ui::rgb(kTaskbarDivider));

    for (int i = 0; i < layout.windowCount; ++i) {
        const std::WindowInfo* window = window_by_id(windows, windowCount, layout.windowIds[i]);
        if (!window) {
            continue;
        }
        const int x = layout.windowsX + i * layout.windowWidth;
        const bool focused = (window->state & std::WindowStateFocused) != 0;
        const bool minimized = (window->state & std::WindowStateMinimized) != 0;
        if (focused) {
            canvas.fill_rounded({ x + 3, center.y + 11, layout.windowWidth - 6, center.height - 22 }, 14,
                                ui::rgb(kTaskbarButtonFocused));
        }
        char title[sizeof(window->title)];
        const int titleWidth = font.fit(window->title, layout.windowWidth - 18, title, sizeof(title));
        canvas.text(x + (layout.windowWidth - titleWidth) / 2, center.y + (center.height / 2) + 6, title, font,
                    ui::rgb(minimized ? kTaskbarDimText : kTitleText));
    }
}

// Whether a window of the slot's app is open.
static bool slot_running(int slot, const std::WindowInfo* windows, std::uint64_t windowCount) {
    const char* title = taskbar_slot_window_title(slot);
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        if ((windows[i].state & std::WindowStateClosed) == 0 &&
            std::strncmp(windows[i].title, title, sizeof(windows[i].title)) == 0) {
            return true;
        }
    }
    return false;
}

void draw_taskbar(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                  const ui::Image* brandImage, const TaskbarStatusAssets* statusIcons) {
    const std::uint32_t screenWidth = static_cast<std::uint32_t>(canvas.width());
    const std::uint32_t screenHeight = static_cast<std::uint32_t>(canvas.height());
    const ui::Font& font = ui::Font::ui();
    const ui::Color color = ui::rgb(kTaskbarColor);
    const TaskbarLayout layout = taskbar_layout(screenWidth, screenHeight, windows, windowCount);
    for (int index = 0; index < 3; index += 2) {
        canvas.fill_rounded(taskbar_rect_at(screenWidth, screenHeight, index), kTaskbarCornerRadius, color);
    }
    const Rect& centerRect = layout.center;
    canvas.fill_rounded(centerRect, kTaskbarCornerRadius, color);

    const ui::Color labelColor = ui::rgb(kTitleText);
    const ui::Color markColor = ui::rgb(kTaskbarRunningMark);
    for (int slot = 0; slot < kTaskbarCenterSlotCount; ++slot) {
        const int slotX = centerRect.x + slot * layout.launcherWidth;
        const char* label = taskbar_slot_label(slot);
        canvas.text(slotX + (layout.launcherWidth - font.width(label)) / 2,
                    centerRect.y + (centerRect.height / 2) + 6, label, font, labelColor);
        if (slot_running(slot, windows, windowCount)) {
            canvas.fill_rounded({ slotX + (layout.launcherWidth - 14) / 2, centerRect.y + centerRect.height - 13, 14, 4 },
                                2, markColor);
        }
    }
    if (layout.windowCount > 0) {
        draw_window_buttons(canvas, layout, windows, windowCount);
    }

    if (brandImage && brandImage->valid()) {
        const Rect leftRect = taskbar_rect_at(screenWidth, screenHeight, 0);
        canvas.image(*brandImage, Rect { leftRect.x + kTaskbarBrandOffsetX,
                                         leftRect.y + ((leftRect.height - kTaskbarBrandHeight) / 2),
                                         kTaskbarBrandWidth, kTaskbarBrandHeight });
    }

    const Rect rightRect = taskbar_rect_at(screenWidth, screenHeight, 2);
    if (statusIcons) {
        const int iconY = rightRect.y + ((rightRect.height - kTaskbarStatusIconSize) / 2);
        if (statusIcons->noInternet.valid() && !gNetworkOnline) {
            canvas.image(statusIcons->noInternet, Rect { rightRect.x + kTaskbarStatusNoInternetOffsetX, iconY,
                                                         kTaskbarStatusIconSize, kTaskbarStatusIconSize });
        }
        if (statusIcons->noSound.valid()) {
            canvas.image(statusIcons->noSound, Rect { rightRect.x + kTaskbarStatusNoSoundOffsetX, iconY,
                                                      kTaskbarStatusIconSize, kTaskbarStatusIconSize });
        }
    }

    const std::Time::DateTime now = std::Time::now();
    const std::Time::TimeString timeText = std::Time::time_string(now);
    const std::Time::DateString dateText = std::Time::date_string(now);
    const ui::Color textColor = ui::rgb(kTitleText);
    const ui::Font& timeFont = gClockTimeFont.valid() ? gClockTimeFont : font;
    const ui::Font& dateFont = gClockDateFont.valid() ? gClockDateFont : font;
    canvas.text(rightRect.x + rightRect.width - kTaskbarClockRightPadding - timeFont.width(timeText.value),
                rightRect.y + kTaskbarClockTimeBaselineOffsetY, timeText.value, timeFont, textColor);
    canvas.text(rightRect.x + rightRect.width - kTaskbarClockRightPadding - dateFont.width(dateText.value),
                rightRect.y + kTaskbarClockDateBaselineOffsetY, dateText.value, dateFont, textColor);
}

// Everything the canvas's clip covers: wallpaper, the windows that reach
// into it (bottom to top), the taskbar.
static void draw_scene(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                       SurfaceCacheEntry* cache, const DesktopBackground* background,
                       const ui::Image* brandImage, const TaskbarStatusAssets* statusIcons,
                       const WindowControlAssets* controls) {
    const std::uint64_t redrawStart = std::gettime();
    std::uint64_t phaseStart = std::gettime();
    draw_background(canvas, background);
    add_elapsed(phaseStart, &gTiming.backgroundMs);

    const Rect clip = canvas.clip();
    for (std::uint64_t i = 0; i < windowCount; ++i) {
        if (!is_window_visible(windows[i])) {
            continue;
        }

        const Rect frame = { windows[i].x, windows[i].y, frame_width(windows[i]), frame_height(windows[i]) };
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

    draw_taskbar(canvas, windows, windowCount, brandImage, statusIcons);
    add_elapsed(redrawStart, &gTiming.sceneRedrawMs);
}

void redraw_scene(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                  SurfaceCacheEntry* cache, const DesktopBackground* background,
                  const ui::Image* brandImage, const TaskbarStatusAssets* statusIcons,
                  const WindowControlAssets* controls) {
    draw_scene(canvas, windows, windowCount, cache, background, brandImage, statusIcons, controls);
}

void redraw_scene_rect(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount,
                       SurfaceCacheEntry* cache, const DesktopBackground* background,
                       const Rect& dirty, const ui::Image* brandImage, const TaskbarStatusAssets* statusIcons,
                       const WindowControlAssets* controls) {
    ui::Canvas clipped = canvas.clipped(dirty);
    if (clipped.clip().empty()) {
        return;
    }
    draw_scene(clipped, windows, windowCount, cache, background, brandImage, statusIcons, controls);
}
