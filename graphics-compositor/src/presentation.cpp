#include "common.hpp"

Rect cursor_rect(const CursorState& cursor, const CursorImage& cursorImage) {
    if (!cursorImage.ready()) {
        return { cursor.x, cursor.y, 0, 0 };
    }
    return { cursor.x - cursorImage.hotspotX, cursor.y - cursorImage.hotspotY,
             cursorImage.image.width(), cursorImage.image.height() };
}

Rect clamp_rect(const Rect& rect, std::uint32_t width, std::uint32_t height) {
    return rect.intersect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
}

void present_rect_to_framebuffer(const ui::Canvas& source, const FramebufferView& target, const Rect& rect) {
    const std::uint64_t presentStart = std::gettime();
    const std::uint32_t sourceWidth = static_cast<std::uint32_t>(source.width());
    const std::uint32_t sourceHeight = static_cast<std::uint32_t>(source.height());
    const Rect clipped = clamp_rect(rect, sourceWidth < target.width ? sourceWidth : target.width,
        sourceHeight < target.height ? sourceHeight : target.height);
    if (clipped.empty()) {
        return;
    }

    for (int y = 0; y < clipped.height; ++y) {
        const int srcY = clipped.y + y;
        const int srcX = clipped.x;
        present_texel_row_to_framebuffer(
            &source.pixels()[srcY * source.pitch() + srcX],
            &target.pixels[srcY * target.pitch + srcX],
            static_cast<std::uint32_t>(clipped.width)
        );
    }

    std::fb_flush(
        static_cast<std::uint32_t>(clipped.x),
        static_cast<std::uint32_t>(clipped.y),
        static_cast<std::uint32_t>(clipped.width),
        static_cast<std::uint32_t>(clipped.height)
    );
    add_elapsed(presentStart, &gTiming.presentMs);
}

// The pointer is drawn straight onto the framebuffer over the finished scene,
// so moving it only repaints where it was and where it is.
void present_cursor_move(const ui::Canvas& scene, const FramebufferView& fb, const CursorState& oldCursor, const CursorState& newCursor, const CursorImage& cursorImage) {
    const std::uint64_t presentStart = std::gettime();
    const Rect dirty = clamp_rect(cursor_rect(oldCursor, cursorImage).unite(cursor_rect(newCursor, cursorImage)), fb.width, fb.height);
    if (dirty.empty()) {
        return;
    }

    const ui::Color* cursorPixels = cursorImage.image.pixels();
    const int cursorWidth = cursorImage.image.width();
    const int cursorHeight = cursorImage.image.height();
    for (int y = 0; y < dirty.height; ++y) {
        const int drawY = dirty.y + y;
        for (int x = 0; x < dirty.width; ++x) {
            const int drawX = dirty.x + x;
            ui::Color color = ui::Color::from_word(scene.pixels()[drawY * scene.pitch() + drawX]);

            const int cursorLocalX = drawX - (newCursor.x - cursorImage.hotspotX);
            const int cursorLocalY = drawY - (newCursor.y - cursorImage.hotspotY);
            if (cursorImage.ready() &&
                cursorLocalX >= 0 && cursorLocalY >= 0 &&
                cursorLocalX < cursorWidth && cursorLocalY < cursorHeight) {
                const ui::Color& src = cursorPixels[cursorLocalY * cursorWidth + cursorLocalX];
                if (src.a != 0) {
                    color = src.a == 255 ? src : ui::blend(color, src, src.a);
                }
            }

            fb.pixels[drawY * fb.pitch + drawX] = color.word() & 0x00FFFFFFU;
        }
    }

    std::fb_flush(
        static_cast<std::uint32_t>(dirty.x),
        static_cast<std::uint32_t>(dirty.y),
        static_cast<std::uint32_t>(dirty.width),
        static_cast<std::uint32_t>(dirty.height)
    );
    add_elapsed(presentStart, &gTiming.presentMs);
}

void move_cursor(const FramebufferView& fb, CursorState* cursor, int x, int y) {
    if (!cursor) {
        return;
    }

    int maxX = static_cast<int>(fb.width) - 1;
    int maxY = static_cast<int>(fb.height) - 1;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    cursor->x = x < 0 ? 0 : (x > maxX ? maxX : x);
    cursor->y = y < 0 ? 0 : (y > maxY ? maxY : y);
}
