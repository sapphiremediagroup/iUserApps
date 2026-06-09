#include "common.hpp"

void fill_circle(const RenderBuffer& buffer, int centerX, int centerY, int radius, const RGBATexel& color) {
    if (radius <= 0) {
        return;
    }

    static constexpr int kSampleCount = 8;
    static constexpr int kSampleTotal = kSampleCount * kSampleCount;
    const int startX = centerX - radius - 1;
    const int endX = centerX + radius + 1;
    const int startY = centerY - radius - 1;
    const int endY = centerY + radius + 1;
    const double radiusSquared = static_cast<double>(radius * radius);

    for (int drawY = startY; drawY <= endY; ++drawY) {
        for (int drawX = startX; drawX <= endX; ++drawX) {
            int inside = 0;
            for (int sy = 0; sy < kSampleCount; ++sy) {
                for (int sx = 0; sx < kSampleCount; ++sx) {
                    const double sampleX = (static_cast<double>(drawX) + ((static_cast<double>(sx) + 0.5) / kSampleCount)) - static_cast<double>(centerX);
                    const double sampleY = (static_cast<double>(drawY) + ((static_cast<double>(sy) + 0.5) / kSampleCount)) - static_cast<double>(centerY);
                    if ((sampleX * sampleX) + (sampleY * sampleY) <= radiusSquared) {
                        ++inside;
                    }
                }
            }

            if (inside == 0) {
                continue;
            }

            const std::uint8_t alpha = static_cast<std::uint8_t>((inside * 255 + (kSampleTotal / 2)) / kSampleTotal);
            blend_pixel(buffer, drawX, drawY, color, alpha);
        }
    }
}

void draw_cursor(const RenderBuffer& buffer, const CursorState& cursor, const CursorImage& cursorImage) {
    if (!cursorImage.ready || !cursorImage.pixels) {
        return;
    }

    const int originX = cursor.x - cursorImage.hotspotX;
    const int originY = cursor.y - cursorImage.hotspotY;
    for (int y = 0; y < cursorImage.height; ++y) {
        for (int x = 0; x < cursorImage.width; ++x) {
            const int drawX = originX + x;
            const int drawY = originY + y;
            if (drawX < 0 || drawY < 0 ||
                drawX >= static_cast<int>(buffer.width) || drawY >= static_cast<int>(buffer.height)) {
                continue;
            }

            const RGBATexel& src = cursorImage.pixels[y * cursorImage.width + x];
            if (src.a == 0) {
                continue;
            }
            blend_pixel(buffer, drawX, drawY, src, src.a);
        }
    }
}

Rect cursor_rect(const CursorState& cursor, const CursorImage& cursorImage) {
    if (!cursorImage.ready) {
        return { cursor.x, cursor.y, 0, 0 };
    }
    return { cursor.x - cursorImage.hotspotX, cursor.y - cursorImage.hotspotY, cursorImage.width, cursorImage.height };
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

Rect intersect_rect(const Rect& a, const Rect& b) {
    const int left = a.x > b.x ? a.x : b.x;
    const int top = a.y > b.y ? a.y : b.y;
    const int rightA = a.x + a.width;
    const int rightB = b.x + b.width;
    const int bottomA = a.y + a.height;
    const int bottomB = b.y + b.height;
    const int right = rightA < rightB ? rightA : rightB;
    const int bottom = bottomA < bottomB ? bottomA : bottomB;
    if (right <= left || bottom <= top) {
        return { 0, 0, 0, 0 };
    }
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
    const std::uint64_t presentStart = std::gettime();
    const Rect clipped = clamp_rect(rect, source.width < target.width ? source.width : target.width,
        source.height < target.height ? source.height : target.height);
    if (rect_is_empty(clipped)) {
        return;
    }

    for (int y = 0; y < clipped.height; ++y) {
        const int srcY = clipped.y + y;
        const int srcX = clipped.x;
        present_texel_row_to_framebuffer(
            &source.pixels[srcY * source.pitch + srcX],
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

void draw_cursor_to_framebuffer(const FramebufferView& fb, const CursorState& cursor, const CursorImage& cursorImage) {
    if (!cursorImage.ready || !cursorImage.pixels) {
        return;
    }

    const int originX = cursor.x - cursorImage.hotspotX;
    const int originY = cursor.y - cursorImage.hotspotY;
    for (int y = 0; y < cursorImage.height; ++y) {
        for (int x = 0; x < cursorImage.width; ++x) {
            const int drawX = originX + x;
            const int drawY = originY + y;
            if (drawX < 0 || drawY < 0 ||
                drawX >= static_cast<int>(fb.width) || drawY >= static_cast<int>(fb.height)) {
                continue;
            }

            const RGBATexel& src = cursorImage.pixels[y * cursorImage.width + x];
            if (src.a == 0) {
                continue;
            }
            const std::uint32_t base = fb.pixels[drawY * fb.pitch + drawX];
            RGBATexel dst = {};
            dst.r = static_cast<std::uint8_t>((base >> 16) & 0xFFU);
            dst.g = static_cast<std::uint8_t>((base >> 8) & 0xFFU);
            dst.b = static_cast<std::uint8_t>(base & 0xFFU);
            dst.a = 0xFFU;
            fb.pixels[drawY * fb.pitch + drawX] = rgb_to_screen(src.a == 255 ? src : blend(dst, src, src.a));
        }
    }
}

void present_cursor_move(const RenderBuffer& scene, const FramebufferView& fb, const CursorState& oldCursor, const CursorState& newCursor, const CursorImage& cursorImage) {
    const std::uint64_t presentStart = std::gettime();
    const Rect dirty = clamp_rect(union_rect(cursor_rect(oldCursor, cursorImage), cursor_rect(newCursor, cursorImage)), fb.width, fb.height);
    if (rect_is_empty(dirty)) {
        return;
    }

    for (int y = 0; y < dirty.height; ++y) {
        const int drawY = dirty.y + y;
        for (int x = 0; x < dirty.width; ++x) {
            const int drawX = dirty.x + x;
            RGBATexel color = texel_word_to_rgba(scene.pixels[drawY * scene.pitch + drawX]);

            const int cursorLocalX = drawX - (newCursor.x - cursorImage.hotspotX);
            const int cursorLocalY = drawY - (newCursor.y - cursorImage.hotspotY);
            if (cursorImage.ready &&
                cursorLocalX >= 0 && cursorLocalY >= 0 &&
                cursorLocalX < cursorImage.width && cursorLocalY < cursorImage.height) {
                const RGBATexel& src = cursorImage.pixels[cursorLocalY * cursorImage.width + cursorLocalX];
                if (src.a != 0) {
                    color = src.a == 255 ? src : blend(color, src, src.a);
                }
            }

            fb.pixels[drawY * fb.pitch + drawX] = rgb_to_screen(color);
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
