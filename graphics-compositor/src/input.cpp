#include "common.hpp"

const std::WindowInfo* top_window_at(const std::WindowInfo* windows, std::uint64_t count, int x, int y) {
    for (std::uint64_t i = count; i > 0; --i) {
        const std::WindowInfo& window = windows[i - 1];
        if (!is_window_visible(window)) {
            continue;
        }

        if (point_in_rounded_rect(x, y, window.x, window.y, frame_width(window), frame_height(window), kWindowCornerRadius)) {
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

Rect window_frame_rect(const std::WindowInfo& window) {
    return { window.x, window.y, frame_width(window), frame_height(window) };
}

bool pointer_on_close(const std::WindowInfo& window, int x, int y) {
    const Rect rect = window_control_rect(window, 0);
    return point_in_rect(x, y, rect.x, rect.y, rect.width, rect.height);
}

bool pointer_on_maximize(const std::WindowInfo& window, int x, int y) {
    const Rect rect = window_control_rect(window, 1);
    return point_in_rect(x, y, rect.x, rect.y, rect.width, rect.height);
}

bool pointer_on_minimize(const std::WindowInfo& window, int x, int y) {
    const Rect rect = window_control_rect(window, 2);
    return point_in_rect(x, y, rect.x, rect.y, rect.width, rect.height);
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
    return point_in_top_rounded_rect(x, y, window.x + kBorder, window.y + kBorder, window.width, kTitleBarHeight - kBorder,
                                     kWindowCornerRadius > kBorder ? (kWindowCornerRadius - kBorder) : 0);
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
                *windowDirty = union_rect(*windowDirty, union_rect(oldFrame, newFrame));
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
                *windowDirty = union_rect(*windowDirty, union_rect(oldFrame, newFrame));
            }
        }
    }

    if (!leftWasDown && leftIsDown) {
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
        } else {
            const int slot = taskbar_center_slot_at(screenWidth, screenHeight, event.pointer.x, event.pointer.y);
            if (slot >= 0) {
                if (!launch_taskbar_slot(slot)) {
                    write_str("[graphics.compositor] FAIL spawn taskbar slot\n");
                }
                clear_drag(drag);
                return false;
            }
        }
    }

    if (leftWasDown && !leftIsDown) {
        clear_drag(drag);
    }

    return fullRedraw;
}
