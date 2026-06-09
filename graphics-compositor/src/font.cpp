#define STB_TRUETYPE_IMPLEMENTATION
#include "common.hpp"

namespace graphics_compositor_stb {
static double cuberoot(double value) {
    if (value == 0.0) {
        return 0.0;
    }

    const double sign = value < 0.0 ? -1.0 : 1.0;
    double guess = std::fabs(value);
    if (guess < 1.0) {
        guess = 1.0;
    }

    for (int iteration = 0; iteration < 24; ++iteration) {
        guess = (2.0 * guess + std::fabs(value) / (guess * guess)) / 3.0;
    }
    return sign * guess;
}

double fmod(double value, double modulus) {
    return std::fmod(value, modulus);
}

double pow(double value, double exponent) {
    const double oneThird = 1.0 / 3.0;
    const double diff = exponent - oneThird;
    if (diff > -0.000001 && diff < 0.000001) {
        return cuberoot(value);
    }

    if (exponent == 0.0) {
        return 1.0;
    }
    if (exponent == 1.0) {
        return value;
    }

    const double integer = std::floor(exponent);
    if (exponent == integer) {
        const bool negative = integer < 0.0;
        std::uint64_t count = static_cast<std::uint64_t>(negative ? -integer : integer);
        double result = 1.0;
        for (std::uint64_t index = 0; index < count; ++index) {
            result *= value;
        }
        return negative ? (result == 0.0 ? 0.0 : 1.0 / result) : result;
    }

    return 1.0;
}

double acos(double value) {
    if (value <= -1.0) {
        return 3.14159265358979323846;
    }
    if (value >= 1.0) {
        return 0.0;
    }
    return static_cast<double>(acosf(static_cast<float>(value)));
}
}

namespace {
constexpr int kFirstCachedGlyph = 32;
constexpr int kLastCachedGlyph = 126;

void release_font_storage(UIFont* font) {
    if (!font) {
        return;
    }

    for (std::size_t index = 0; index < sizeof(font->glyphs) / sizeof(font->glyphs[0]); ++index) {
        delete[] font->glyphs[index].alpha;
        font->glyphs[index].alpha = nullptr;
    }

    delete[] font->data;
    std::memset(font, 0, sizeof(*font));
}

bool cache_font_glyphs(UIFont* font) {
    if (!font) {
        return false;
    }

    const float glyphScale = font->scale * static_cast<float>(kFontSupersampleScale);
    for (int ch = kFirstCachedGlyph; ch <= kLastCachedGlyph; ++ch) {
        CachedGlyph& glyph = font->glyphs[ch - kFirstCachedGlyph];

        int advance = 0;
        int leftSideBearing = 0;
        stbtt_GetCodepointHMetrics(&font->info, ch, &advance, &leftSideBearing);
        (void) leftSideBearing;
        glyph.advance = static_cast<int>(advance * font->scale + 0.999f);
        if (glyph.advance <= 0) {
            glyph.advance = 6;
        }

        int width = 0;
        int height = 0;
        int xOffset = 0;
        int yOffset = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(
            &font->info,
            glyphScale,
            glyphScale,
            ch,
            &width,
            &height,
            &xOffset,
            &yOffset
        );
        if (!bitmap || width <= 0 || height <= 0) {
            if (bitmap) {
                stbtt_FreeBitmap(bitmap, nullptr);
            }
            continue;
        }

        glyph.width = (width + kFontSupersampleScale - 1) / kFontSupersampleScale;
        glyph.height = (height + kFontSupersampleScale - 1) / kFontSupersampleScale;
        glyph.xOffset = floor_div_int(xOffset, kFontSupersampleScale);
        glyph.yOffset = floor_div_int(yOffset, kFontSupersampleScale);

        const std::size_t alphaSize = static_cast<std::size_t>(glyph.width) * static_cast<std::size_t>(glyph.height);
        glyph.alpha = new (std::nothrow) std::uint8_t[alphaSize];
        if (!glyph.alpha) {
            glyph.width = 0;
            glyph.height = 0;
            stbtt_FreeBitmap(bitmap, nullptr);
            continue;
        }

        for (int y = 0; y < glyph.height; ++y) {
            for (int x = 0; x < glyph.width; ++x) {
                glyph.alpha[y * glyph.width + x] = downsample_glyph_alpha(bitmap, width, height, x, y);
            }
        }

        stbtt_FreeBitmap(bitmap, nullptr);
    }

    return true;
}
}

bool initialize_font(UIFont* font, const char* path, std::uint32_t pixelHeight) {
    if (!font) {
        return false;
    }

    std::memset(font, 0, sizeof(*font));
    std::Stat stat = {};
    if (!path || std::stat(path, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
        return false;
    }

    const std::size_t fontSize = static_cast<std::size_t>(stat.st_size);
    font->data = new (std::nothrow) unsigned char[fontSize];
    if (!font->data) {
        std::close(file);
        return false;
    }

    std::size_t totalRead = 0;
    while (totalRead < fontSize) {
        const std::uint64_t bytesRead = std::read(file, font->data + totalRead, fontSize - totalRead);
        if (bytesRead == fail || bytesRead == 0) {
            delete[] font->data;
            font->data = nullptr;
            std::close(file);
            std::memset(font, 0, sizeof(*font));
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);

    const int fontOffset = stbtt_GetFontOffsetForIndex(font->data, 0);
    if (fontOffset < 0 || stbtt_InitFont(&font->info, font->data, fontOffset) == 0) {
        release_font_storage(font);
        return false;
    }

    font->scale = stbtt_ScaleForPixelHeight(&font->info, static_cast<float>(pixelHeight));
    if (font->scale <= 0.0f) {
        release_font_storage(font);
        return false;
    }

    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->lineGap);
    font->baseline = static_cast<int>(font->ascent * font->scale + 0.999f);
    font->lineHeight = static_cast<int>(((font->ascent - font->descent + font->lineGap) * font->scale) + 0.999f);
    if (font->lineHeight <= 0) {
        font->lineHeight = static_cast<int>(pixelHeight) + 4;
    }

    if (!cache_font_glyphs(font)) {
        release_font_storage(font);
        return false;
    }

    font->valid = true;
    return true;
}

bool initialize_ui_font(UIFont* font) {
    return initialize_font(font, kUIFontPath, kUIFontPixelHeight);
}

void destroy_ui_font(UIFont* font) {
    if (!font) {
        return;
    }

    release_font_storage(font);
}

int text_width(UIFont& font, const char* text) {
    if (!font.valid || !text) {
        return 0;
    }

    int width = 0;
    for (std::size_t index = 0; text[index] != '\0'; ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < 32 || ch > 126) {
            width += 6;
            continue;
        }

        const CachedGlyph& glyph = font.glyphs[ch - kFirstCachedGlyph];
        width += glyph.advance > 0 ? glyph.advance : 6;
    }

    return width;
}

int floor_div_int(int value, int divisor) {
    if (divisor <= 0) {
        return value;
    }
    if (value >= 0) {
        return value / divisor;
    }
    return -(((-value) + divisor - 1) / divisor);
}

std::uint8_t downsample_glyph_alpha(const unsigned char* bitmap, int width, int height, int x, int y) {
    if (!bitmap || width <= 0 || height <= 0) {
        return 0;
    }

    int alphaSum = 0;
    int sampleCount = 0;
    const int sampleX = x * kFontSupersampleScale;
    const int sampleY = y * kFontSupersampleScale;
    for (int subY = 0; subY < kFontSupersampleScale; ++subY) {
        const int sourceY = sampleY + subY;
        if (sourceY >= height) {
            continue;
        }
        for (int subX = 0; subX < kFontSupersampleScale; ++subX) {
            const int sourceX = sampleX + subX;
            if (sourceX >= width) {
                continue;
            }
            alphaSum += bitmap[sourceY * width + sourceX];
            ++sampleCount;
        }
    }

    if (sampleCount == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>((alphaSum + (sampleCount / 2)) / sampleCount);
}

void draw_text(const RenderBuffer& buffer, UIFont& font, int x, int y, const char* text, const RGBATexel& color) {
    if (!font.valid || !text) {
        return;
    }

    int penX = x;
    int baselineY = y;
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '\n') {
            penX = x;
            baselineY += font.lineHeight;
            continue;
        }
        if (ch < 32 || ch > 126) {
            penX += 6;
            continue;
        }

        const CachedGlyph& glyph = font.glyphs[ch - kFirstCachedGlyph];
        if (!glyph.alpha || glyph.width <= 0 || glyph.height <= 0) {
            penX += glyph.advance > 0 ? glyph.advance : 6;
            continue;
        }

        const int startX = penX + glyph.xOffset;
        const int startY = baselineY + glyph.yOffset;

        for (int drawY = 0; drawY < glyph.height; ++drawY) {
            const int dstY = startY + drawY;
            if (dstY < 0 || dstY >= static_cast<int>(buffer.height)) {
                continue;
            }

            for (int drawX = 0; drawX < glyph.width; ++drawX) {
                const int dstX = startX + drawX;
                if (dstX < 0 || dstX >= static_cast<int>(buffer.width)) {
                    continue;
                }

                const std::uint8_t alpha = glyph.alpha[drawY * glyph.width + drawX];
                if (alpha == 0) {
                    continue;
                }

                if (gDrawClipEnabled &&
                    (dstX < gDrawClip.x || dstY < gDrawClip.y ||
                     dstX >= gDrawClip.x + gDrawClip.width || dstY >= gDrawClip.y + gDrawClip.height)) {
                    continue;
                }

                std::uint32_t& dst = buffer.pixels[dstY * buffer.pitch + dstX];
                dst = texel_to_word(blend(texel_word_to_rgba(dst), color, alpha));
            }
        }

        penX += glyph.advance > 0 ? glyph.advance : 6;
    }
}
