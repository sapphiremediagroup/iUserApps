#include "common.hpp"

// Inside the rectangle whose top corners are rounded by `radius` (a title
// bar); the radius is clamped like the drawing's.
static bool in_top_rounded_rect(const Rect& rect, int radius, int x, int y) {
    if (rect.empty() || !rect.contains(x, y)) {
        return false;
    }
    if (radius > rect.width / 2) {
        radius = rect.width / 2;
    }
    if (radius > rect.height / 2) {
        radius = rect.height / 2;
    }
    if (radius <= 0 || y >= rect.y + radius) {
        return true;
    }
    const int leftCenterX = rect.x + radius;
    const int rightCenterX = rect.x + rect.width - radius - 1;
    if (x >= leftCenterX && x <= rightCenterX) {
        return true;
    }
    const int dx = x - (x < leftCenterX ? leftCenterX : rightCenterX);
    const int dy = y - (rect.y + radius);
    return dx * dx + dy * dy <= radius * radius;
}

const std::WindowInfo* top_window_at(const std::WindowInfo* windows, std::uint64_t count, int x, int y) {
    for (std::uint64_t i = count; i > 0; --i) {
        const std::WindowInfo& window = windows[i - 1];
        if (!is_window_visible(window)) {
            continue;
        }

        if (ui::in_rounded_rect(window_frame_rect(window), kWindowCornerRadius, x, y)) {
            return &window;
        }
    }

    return nullptr;
}

const std::WindowInfo* window_by_id(const std::WindowInfo* windows, std::uint64_t count, std::uint64_t id) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (windows[i].id == id) {
            return &windows[i];
        }
    }
    return nullptr;
}

// Pointer input for the apps: the window under the pointer (or the one
// holding it since a press in its content) gets the event in its own
// coordinates. Title bars, frames, drags and the taskbar stay ours.
void route_pointer_to_window(const std::Event& event, const std::WindowInfo* windows, std::uint64_t count,
                             std::uint32_t screenWidth, std::uint32_t screenHeight,
                             const DragState& drag, PointerRoute* route) {
    if (event.type != std::EventType::Pointer || drag.moving || drag.resizing) {
        return;
    }

    const std::WindowInfo* target = route->grabWindowId != 0
        ? window_by_id(windows, count, route->grabWindowId) : nullptr;
    if (!target) {
        route->grabWindowId = 0;
        // The taskbar is drawn over the windows, so it is the taskbar's.
        const TaskbarLayout layout = taskbar_layout(screenWidth, screenHeight, windows, count);
        if (taskbar_hit(layout, event.pointer.x, event.pointer.y).kind != TaskbarHitKind::None) {
            return;
        }
        const std::WindowInfo* top = top_window_at(windows, count, event.pointer.x, event.pointer.y);
        if (top && Rect { top->x + kBorder, top->y + kTitleBarHeight, top->width, top->height }
                       .contains(event.pointer.x, event.pointer.y)) {
            target = top;
        }
    }
    if (!target) {
        return;
    }

    if (event.pointer.action == std::PointerEventAction::Button) {
        route->grabWindowId = event.pointer.buttons != 0 ? target->id : 0;
    }

    std::Event local = event;
    local.pointer.x = event.pointer.x - (target->x + kBorder);
    local.pointer.y = event.pointer.y - (target->y + kTitleBarHeight);
    std::window_post_event(target->id, &local);
}

Rect window_frame_rect(const std::WindowInfo& window) {
    return { window.x, window.y, frame_width(window), frame_height(window) };
}

Rect window_damage_rect(const std::WindowInfo& window) {
    const Rect frame = window_frame_rect(window);
    return { frame.x - kShadowMargin, frame.y - kShadowMargin, frame.width + 2 * kShadowMargin,
             frame.height + 2 * kShadowMargin };
}

int window_control_at(const std::WindowInfo& window, int x, int y) {
    for (int control = kControlClose; control <= kControlMinimize; ++control) {
        if (window_control_rect(window, control).contains(x, y)) {
            return control;
        }
    }
    return -1;
}

bool pointer_on_close(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, kControlClose).contains(x, y);
}

bool pointer_on_maximize(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, kControlMaximize).contains(x, y);
}

bool pointer_on_minimize(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, kControlMinimize).contains(x, y);
}

// Follows the window control under the pointer; a change repaints the old
// and the new button.
static void update_control_hover(const std::Event& event, const std::WindowInfo* windows, std::uint64_t windowCount,
                                 std::uint32_t screenWidth, std::uint32_t screenHeight, const DragState* drag,
                                 Rect* windowDirty) {
    WindowHover hover = {};
    const TaskbarLayout layout = taskbar_layout(screenWidth, screenHeight, windows, windowCount);
    const TaskbarHit taskbarHit = taskbar_hit(layout, event.pointer.x, event.pointer.y);
    // The tile under the pointer shows its tooltip.
    const int tile = !drag->moving && !drag->resizing && taskbarHit.kind == TaskbarHitKind::Tile ? taskbarHit.tile : -1;
    if (tile != gTaskbarHover) {
        if (windowDirty) {
            const Rect before = taskbar_tooltip_rect(layout, gTaskbarHover, windows, windowCount);
            const Rect after = taskbar_tooltip_rect(layout, tile, windows, windowCount);
            *windowDirty = windowDirty->unite(before).unite(after);
            if (gTaskbarHover >= 0 && gTaskbarHover < layout.tileCount) {
                *windowDirty = windowDirty->unite(layout.tiles[gTaskbarHover].rect);
            }
            if (tile >= 0) {
                *windowDirty = windowDirty->unite(layout.tiles[tile].rect);
            }
        }
        gTaskbarHover = tile;
    }
    if (!drag->moving && !drag->resizing && taskbarHit.kind == TaskbarHitKind::None) {
        const std::WindowInfo* top = top_window_at(windows, windowCount, event.pointer.x, event.pointer.y);
        const int control = top ? window_control_at(*top, event.pointer.x, event.pointer.y) : -1;
        if (control >= 0) {
            hover = { top->id, control };
        }
    }
    if (hover.windowId == gWindowHover.windowId && hover.control == gWindowHover.control) {
        return;
    }
    if (windowDirty) {
        const std::WindowInfo* before = window_by_id(windows, windowCount, gWindowHover.windowId);
        if (before) {
            *windowDirty = windowDirty->unite(window_control_rect(*before, gWindowHover.control));
        }
        const std::WindowInfo* now = window_by_id(windows, windowCount, hover.windowId);
        if (now) {
            *windowDirty = windowDirty->unite(window_control_rect(*now, hover.control));
        }
    }
    gWindowHover = hover;
}

bool pointer_on_resize_grip(const std::WindowInfo& window, int x, int y) {
    return Rect {
        window.x + frame_width(window) - kBorder - kResizeGripSize,
        window.y + frame_height(window) - kBorder - kResizeGripSize,
        kResizeGripSize,
        kResizeGripSize
    }.contains(x, y);
}

bool pointer_on_titlebar(const std::WindowInfo& window, int x, int y) {
    return in_top_rounded_rect({ window.x + kBorder, window.y + kBorder, window.width, kTitleBarHeight - kBorder },
                               kWindowCornerRadius > kBorder ? (kWindowCornerRadius - kBorder) : 0, x, y);
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

bool handle_pointer_event(const std::Event& event, const std::WindowInfo* windows, std::uint64_t windowCount,
                          std::uint32_t screenWidth, std::uint32_t screenHeight,
                          DragState* drag, std::uint16_t* buttons, Rect* windowDirty) {
    if (event.type != std::EventType::Pointer) {
        return false;
    }

    bool fullRedraw = false;
    update_control_hover(event, windows, windowCount, screenWidth, screenHeight, drag, windowDirty);
    const std::uint16_t previousButtons = *buttons;
    *buttons = event.pointer.buttons;
    const bool leftWasDown = (previousButtons & 0x1u) != 0;
    const bool leftIsDown = (event.pointer.buttons & 0x1u) != 0;

    if (drag->moving && leftIsDown) {
        const std::WindowInfo* before = window_by_id(windows, windowCount, drag->windowId);
        const int newX = event.pointer.x - drag->grabOffsetX;
        const int newY = event.pointer.y - drag->grabOffsetY;
        if (before && std::compositor_move_window(drag->windowId, newX, newY) != fail) {
            const Rect oldFrame = window_damage_rect(*before);
            const Rect newFrame = oldFrame.offset(newX - before->x, newY - before->y);
            if (windowDirty) {
                *windowDirty = windowDirty->unite(oldFrame.unite(newFrame));
            }
        }
    } else if (drag->resizing && leftIsDown) {
        const std::WindowInfo* before = window_by_id(windows, windowCount, drag->windowId);
        int newWidth = drag->startWidth + (event.pointer.x - drag->anchorX);
        int newHeight = drag->startHeight + (event.pointer.y - drag->anchorY);
        if (newWidth < 64) newWidth = 64;
        if (newHeight < 48) newHeight = 48;
        if (before && std::compositor_resize_window(drag->windowId, newWidth, newHeight) != fail) {
            const Rect oldFrame = window_damage_rect(*before);
            const Rect newFrame = { oldFrame.x, oldFrame.y, newWidth + (kBorder * 2) + 2 * kShadowMargin,
                                    newHeight + kTitleBarHeight + kBorder + 2 * kShadowMargin };
            if (windowDirty) {
                *windowDirty = windowDirty->unite(oldFrame.unite(newFrame));
            }
        }
    }

    if (!leftWasDown && leftIsDown) {
        // The taskbar first: it is drawn over the windows.
        const TaskbarLayout layout = taskbar_layout(screenWidth, screenHeight, windows, windowCount);
        const TaskbarHit hit = taskbar_hit(layout, event.pointer.x, event.pointer.y);
        if (hit.kind == TaskbarHitKind::Tile) {
            clear_drag(drag);
            return taskbar_click(layout, hit.tile, windows, windowCount);
        }
        if (hit.kind != TaskbarHitKind::None) {
            clear_drag(drag);
            return false;
        }

        const std::WindowInfo* window = top_window_at(windows, windowCount, event.pointer.x, event.pointer.y);
        if (window) {
            std::compositor_focus_window(window->id);
            fullRedraw = true;

            if (pointer_on_close(*window, event.pointer.x, event.pointer.y)) {
                std::compositor_control_window(window->id, std::WindowControlAction::Close);
                clear_drag(drag);
                return true;
            }
            if (pointer_on_maximize(*window, event.pointer.x, event.pointer.y)) {
                if ((window->state & std::WindowStateMaximized) != 0) {
                    std::compositor_control_window(window->id, std::WindowControlAction::Restore);
                } else {
                    // The frame fills the work area, 8 px in from the screen's
                    // edges and kWorkAreaGap above the taskbar.
                    const int frameX = 8;
                    const int frameY = 8;
                    const int frameWidth = static_cast<int>(screenWidth) - 16;
                    const int frameHeight = work_area_bottom(screenHeight) - frameY;
                    std::compositor_control_window(window->id, std::WindowControlAction::Maximize, frameX, frameY,
                                                   frameWidth - 2 * kBorder, frameHeight - kTitleBarHeight - kBorder);
                }
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

    return fullRedraw;
}
