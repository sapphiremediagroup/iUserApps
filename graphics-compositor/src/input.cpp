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
        if (taskbar_hit(layout, screenWidth, screenHeight, event.pointer.x, event.pointer.y).kind != TaskbarHitKind::None) {
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

bool pointer_on_close(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, 0).contains(x, y);
}

bool pointer_on_maximize(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, 1).contains(x, y);
}

bool pointer_on_minimize(const std::WindowInfo& window, int x, int y) {
    return window_control_rect(window, 2).contains(x, y);
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
    const std::uint16_t previousButtons = *buttons;
    *buttons = event.pointer.buttons;
    const bool leftWasDown = (previousButtons & 0x1u) != 0;
    const bool leftIsDown = (event.pointer.buttons & 0x1u) != 0;

    if (drag->moving && leftIsDown) {
        const std::WindowInfo* before = window_by_id(windows, windowCount, drag->windowId);
        const int newX = event.pointer.x - drag->grabOffsetX;
        const int newY = event.pointer.y - drag->grabOffsetY;
        if (before && std::compositor_move_window(drag->windowId, newX, newY) != fail) {
            const Rect oldFrame = window_frame_rect(*before);
            const Rect newFrame = { newX, newY, oldFrame.width, oldFrame.height };
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
            const Rect oldFrame = window_frame_rect(*before);
            const Rect newFrame = { before->x, before->y, newWidth + (kBorder * 2), newHeight + kTitleBarHeight + kBorder };
            if (windowDirty) {
                *windowDirty = windowDirty->unite(oldFrame.unite(newFrame));
            }
        }
    }

    if (!leftWasDown && leftIsDown) {
        // The taskbar first: it is drawn over the windows.
        const TaskbarLayout layout = taskbar_layout(screenWidth, screenHeight, windows, windowCount);
        const TaskbarHit hit = taskbar_hit(layout, screenWidth, screenHeight, event.pointer.x, event.pointer.y);
        if (hit.kind == TaskbarHitKind::Launcher) {
            if (!launch_taskbar_slot(hit.slot)) {
                write_str("[graphics.compositor] FAIL spawn taskbar slot\n");
            }
            clear_drag(drag);
            return false;
        }
        if (hit.kind == TaskbarHitKind::Window) {
            // A window's button: bring it up, or minimize it if it is the
            // focused one (focusing a minimized window restores it).
            const std::WindowInfo* window = window_by_id(windows, windowCount, hit.windowId);
            if (window && (window->state & std::WindowStateFocused) != 0 &&
                (window->state & std::WindowStateMinimized) == 0) {
                std::compositor_control_window(window->id, std::WindowControlAction::Minimize);
            } else if (window) {
                std::compositor_focus_window(window->id);
            }
            clear_drag(drag);
            return true;
        }
        if (hit.kind == TaskbarHitKind::Pill) {
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

    return fullRedraw;
}
