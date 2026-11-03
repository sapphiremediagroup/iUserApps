#include "common.hpp"

// Loads the pointer image and, when it is larger than kMaxCursorSize, scales
// it down by area averaging with the colours weighted by their alpha (so
// transparent pixels do not darken the edges).
bool CursorImage::loadFromFile(const char* path) {
    image.reset();
    hotspotX = 0;
    hotspotY = 0;

    ui::Image source;
    if (!path || path[0] == '\0' || !source.load(path)) {
        return false;
    }
    const int imageWidth = source.width();
    const int imageHeight = source.height();
    const ui::Color* imagePixels = source.pixels();

    int targetWidth = imageWidth;
    int targetHeight = imageHeight;
    if (targetWidth > kMaxCursorSize || targetHeight > kMaxCursorSize) {
        if (targetWidth >= targetHeight) {
            targetHeight = (targetHeight * kMaxCursorSize) / targetWidth;
            targetWidth = kMaxCursorSize;
        } else {
            targetWidth = (targetWidth * kMaxCursorSize) / targetHeight;
            targetHeight = kMaxCursorSize;
        }
        if (targetWidth < 1) {
            targetWidth = 1;
        }
        if (targetHeight < 1) {
            targetHeight = 1;
        }
    }

    if (!image.create(targetWidth, targetHeight)) {
        return false;
    }
    ui::Color* pixels = image.pixels();

    for (int y = 0; y < targetHeight; ++y) {
        const double srcTop = (static_cast<double>(y) * static_cast<double>(imageHeight)) / static_cast<double>(targetHeight);
        const double srcBottom = (static_cast<double>(y + 1) * static_cast<double>(imageHeight)) / static_cast<double>(targetHeight);
        int startY = static_cast<int>(std::floor(srcTop));
        int endY = static_cast<int>(std::ceil(srcBottom));
        if (startY < 0) {
            startY = 0;
        }
        if (endY > imageHeight) {
            endY = imageHeight;
        }

        for (int x = 0; x < targetWidth; ++x) {
            const double srcLeft = (static_cast<double>(x) * static_cast<double>(imageWidth)) / static_cast<double>(targetWidth);
            const double srcRight = (static_cast<double>(x + 1) * static_cast<double>(imageWidth)) / static_cast<double>(targetWidth);
            int startX = static_cast<int>(std::floor(srcLeft));
            int endX = static_cast<int>(std::ceil(srcRight));
            if (startX < 0) {
                startX = 0;
            }
            if (endX > imageWidth) {
                endX = imageWidth;
            }

            double totalWeight = 0.0;
            double alphaSum = 0.0;
            double redSum = 0.0;
            double greenSum = 0.0;
            double blueSum = 0.0;

            for (int sampleY = startY; sampleY < endY; ++sampleY) {
                const double overlapTop = srcTop > static_cast<double>(sampleY) ? srcTop : static_cast<double>(sampleY);
                const double overlapBottom = srcBottom < static_cast<double>(sampleY + 1) ? srcBottom : static_cast<double>(sampleY + 1);
                const double yWeight = overlapBottom - overlapTop;
                if (yWeight <= 0.0) {
                    continue;
                }

                for (int sampleX = startX; sampleX < endX; ++sampleX) {
                    const double overlapLeft = srcLeft > static_cast<double>(sampleX) ? srcLeft : static_cast<double>(sampleX);
                    const double overlapRight = srcRight < static_cast<double>(sampleX + 1) ? srcRight : static_cast<double>(sampleX + 1);
                    const double xWeight = overlapRight - overlapLeft;
                    if (xWeight <= 0.0) {
                        continue;
                    }

                    const double weight = xWeight * yWeight;
                    const ui::Color& src = imagePixels[sampleY * imageWidth + sampleX];
                    const double alpha = (static_cast<double>(src.a) / 255.0) * weight;
                    totalWeight += weight;
                    alphaSum += alpha;
                    redSum += static_cast<double>(src.r) * alpha;
                    greenSum += static_cast<double>(src.g) * alpha;
                    blueSum += static_cast<double>(src.b) * alpha;
                }
            }

            const double outAlpha = totalWeight > 0.0 ? (alphaSum / totalWeight) : 0.0;
            ui::Color& dst = pixels[y * targetWidth + x];
            if (outAlpha <= 0.0) {
                dst = {};
                continue;
            }

            dst.r = static_cast<std::uint8_t>((redSum / alphaSum) + 0.5);
            dst.g = static_cast<std::uint8_t>((greenSum / alphaSum) + 0.5);
            dst.b = static_cast<std::uint8_t>((blueSum / alphaSum) + 0.5);
            dst.a = static_cast<std::uint8_t>((outAlpha * 255.0) + 0.5);
        }
    }

    return true;
}

// An icon loaded and scaled to size x size; complains on the serial port.
static bool load_icon(ui::Image* icon, const char* path, int size, const char* name) {
    if (!icon->load(path)) {
        write_str("[graphics.compositor] ");
        write_str(name);
        write_str(" icon load failed\n");
        return false;
    }
    if (!icon->resize(size, size)) {
        write_str("[graphics.compositor] ");
        write_str(name);
        write_str(" icon resize failed\n");
        icon->reset();
        return false;
    }
    return true;
}


// A /bin/icons line icon at `size`, in `color`.
static bool load_line_icon(ui::Image* image, const char* name, int size, ui::Color color) {
    char path[96] = {};
    std::size_t length = 0;
    const char* const parts[3] = { kIconDirectory, name, ".svg" };
    for (const char* part : parts) {
        for (std::size_t i = 0; part[i] != '\0' && length + 1 < sizeof(path); ++i) {
            path[length++] = part[i];
        }
    }
    if (!image->load_svg(path, size, size)) {
        write_str("[graphics.compositor] icon load failed: ");
        write_str(path);
        write_str("\n");
        return false;
    }
    image->tint(color);
    return true;
}

bool TaskbarAssets::load() {
    const ui::Theme& t = ui::theme();
    static const char* const kApps[kTitleIconCount] = { "terminal", "folder", "image", "cube", "globe", "app" };
    bool ok = true;
    for (int a = 0; a < kTitleIconCount; ++a) {
        ok = load_line_icon(&apps[a][0], kApps[a], kTileIconSize, t.text) && ok;
        ok = load_line_icon(&apps[a][1], kApps[a], kTileIconSize, t.kindFile) && ok;
    }
    ok = load_line_icon(&wifi, "wifi", kTrayIconSize, t.kindFile) && ok;
    ok = load_line_icon(&wifiOff, "wifi-off", kTrayIconSize, t.kindFile) && ok;
    ok = load_line_icon(&volumeOff, "volume-x", kTrayIconSize, t.kindFile) && ok;
    if (!startFont.load_em(kNotchBoldFontPath, 14.0f)) {
        write_str("[graphics.compositor] start button font load failed\n");
        ok = false;
    }
    return ok;
}

bool WindowControlAssets::load() {
    const ui::Theme& t = ui::theme();
    static const char* const kGlyphs[4] = { "win-close", "win-maximize", "win-minimize", "win-restore" };
    static const char* const kApps[kTitleIconCount] = { "terminal", "folder", "image", "cube", "globe", "app" };
    bool ok = true;
    for (int g = 0; g < 4; ++g) {
        ok = load_line_icon(&glyphs[g][0], kGlyphs[g], kControlGlyphSize, t.text) && ok;
        ok = load_line_icon(&glyphs[g][1], kGlyphs[g], kControlGlyphSize, t.textUnfocused) && ok;
        ok = load_line_icon(&glyphs[g][2], kGlyphs[g], kControlGlyphSize, t.accentText) && ok;
    }
    for (int a = 0; a < kTitleIconCount; ++a) {
        ok = load_line_icon(&apps[a][0], kApps[a], kTitleIconSize, t.accentLight) && ok;
        ok = load_line_icon(&apps[a][1], kApps[a], kTitleIconSize, t.textUnfocused) && ok;
    }
    return ok;
}
