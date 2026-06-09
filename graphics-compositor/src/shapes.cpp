#include "common.hpp"

void fill_rect(const RenderBuffer& buffer, int x, int y, int width, int height, const RGBATexel& color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    int startX = x < 0 ? 0 : x;
    int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(buffer.width)) {
        endX = static_cast<int>(buffer.width);
    }
    if (endY > static_cast<int>(buffer.height)) {
        endY = static_cast<int>(buffer.height);
    }
    if (gDrawClipEnabled) {
        if (startX < gDrawClip.x) {
            startX = gDrawClip.x;
        }
        if (startY < gDrawClip.y) {
            startY = gDrawClip.y;
        }
        const int clipRight = gDrawClip.x + gDrawClip.width;
        const int clipBottom = gDrawClip.y + gDrawClip.height;
        if (endX > clipRight) {
            endX = clipRight;
        }
        if (endY > clipBottom) {
            endY = clipBottom;
        }
    }
    if (startX >= endX || startY >= endY) {
        return;
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        fill_texel_row(
            &buffer.pixels[drawY * buffer.pitch + static_cast<std::uint32_t>(startX)],
            static_cast<std::uint32_t>(endX - startX),
            color
        );
    }
}

void blend_pixel(const RenderBuffer& buffer, int x, int y, const RGBATexel& color, std::uint8_t alpha) {
    if (alpha == 0) {
        return;
    }
    if (x < 0 || y < 0 || x >= static_cast<int>(buffer.width) || y >= static_cast<int>(buffer.height)) {
        return;
    }
    if (gDrawClipEnabled &&
        (x < gDrawClip.x || y < gDrawClip.y ||
         x >= gDrawClip.x + gDrawClip.width || y >= gDrawClip.y + gDrawClip.height)) {
        return;
    }

    std::uint32_t& dst = buffer.pixels[y * buffer.pitch + x];
    dst = alpha == 255 ? texel_to_word(color) : texel_to_word(blend(texel_word_to_rgba(dst), color, alpha));
}

int clamp_radius(int width, int height, int radius) {
    if (radius <= 0) {
        return 0;
    }

    const int maxRadiusX = width / 2;
    const int maxRadiusY = height / 2;
    if (radius > maxRadiusX) {
        radius = maxRadiusX;
    }
    if (radius > maxRadiusY) {
        radius = maxRadiusY;
    }
    return radius;
}

bool point_in_rounded_rect(int x, int y, int rectX, int rectY, int width, int height, int radius) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (x < rectX || y < rectY || x >= rectX + width || y >= rectY + height) {
        return false;
    }

    if (radius <= 0) {
        return true;
    }

    radius = clamp_radius(width, height, radius);

    const int left = rectX + radius;
    const int right = rectX + width - radius - 1;
    const int top = rectY + radius;
    const int bottom = rectY + height - radius - 1;
    if ((x >= left && x <= right) || (y >= top && y <= bottom)) {
        return true;
    }

    const int centerX = x < left ? left : right;
    const int centerY = y < top ? top : bottom;
    const int dx = x - centerX;
    const int dy = y - centerY;
    return (dx * dx) + (dy * dy) <= (radius * radius);
}

bool point_in_top_rounded_rect(int x, int y, int rectX, int rectY, int width, int height, int radius) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (x < rectX || y < rectY || x >= rectX + width || y >= rectY + height) {
        return false;
    }

    if (radius <= 0) {
        return true;
    }

    radius = clamp_radius(width, height, radius);
    if (y >= rectY + radius) {
        return true;
    }

    const int leftCenterX = rectX + radius;
    const int rightCenterX = rectX + width - radius - 1;
    const int centerY = rectY + radius;
    if (x >= leftCenterX && x <= rightCenterX) {
        return true;
    }

    const int centerX = x < leftCenterX ? leftCenterX : rightCenterX;
    const int dx = x - centerX;
    const int dy = y - centerY;
    return (dx * dx) + (dy * dy) <= (radius * radius);
}

void fill_rounded_rect(const RenderBuffer& buffer, int x, int y, int width, int height, int radius, const RGBATexel& color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    radius = clamp_radius(width, height, radius);
    const int startX = x < 0 ? 0 : x;
    const int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(buffer.width)) {
        endX = static_cast<int>(buffer.width);
    }
    if (endY > static_cast<int>(buffer.height)) {
        endY = static_cast<int>(buffer.height);
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            if (radius <= 0 ||
                ((drawX >= x + radius) && (drawX < x + width - radius)) ||
                ((drawY >= y + radius) && (drawY < y + height - radius))) {
                blend_pixel(buffer, drawX, drawY, color, 255);
                continue;
            }

            const std::uint8_t alpha = std::Resizer::alpha_from_coverage(
                std::PackedAntiAliasing::rounded_rect_coverage(x, y, width, height, radius, drawX, drawY)
            );
            if (alpha == 0) {
                continue;
            }
            blend_pixel(buffer, drawX, drawY, color, alpha);
        }
    }
}

void fill_top_rounded_rect(const RenderBuffer& buffer, int x, int y, int width, int height, int radius, const RGBATexel& color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    radius = clamp_radius(width, height, radius);
    const int startX = x < 0 ? 0 : x;
    const int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(buffer.width)) {
        endX = static_cast<int>(buffer.width);
    }
    if (endY > static_cast<int>(buffer.height)) {
        endY = static_cast<int>(buffer.height);
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            if (radius <= 0 || drawY >= y + radius || (drawX >= x + radius && drawX < x + width - radius)) {
                blend_pixel(buffer, drawX, drawY, color, 255);
                continue;
            }

            const std::uint8_t alpha = std::Resizer::alpha_from_coverage(
                std::PackedAntiAliasing::top_rounded_rect_coverage(x, y, width, height, radius, drawX, drawY)
            );
            if (alpha == 0) {
                continue;
            }
            blend_pixel(buffer, drawX, drawY, color, alpha);
        }
    }
}

RGBATexel blend(const RGBATexel& dst, const RGBATexel& src, std::uint8_t alpha) {
    const std::uint32_t inv = 255U - alpha;
    RGBATexel out = {};
    out.r = static_cast<std::uint8_t>((dst.r * inv + src.r * alpha) / 255U);
    out.g = static_cast<std::uint8_t>((dst.g * inv + src.g * alpha) / 255U);
    out.b = static_cast<std::uint8_t>((dst.b * inv + src.b * alpha) / 255U);
    out.a = 0xFFU;
    return out;
}
