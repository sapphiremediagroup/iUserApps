#include "common.hpp"

RGBATexel rgba_from_rgb(std::uint32_t color) {
    RGBATexel pixel = {};
    pixel.r = static_cast<std::uint8_t>((color >> 16) & 0xFFU);
    pixel.g = static_cast<std::uint8_t>((color >> 8) & 0xFFU);
    pixel.b = static_cast<std::uint8_t>(color & 0xFFU);
    pixel.a = 0xFFU;
    return pixel;
}

bool has_png_suffix(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length < 4) {
        return false;
    }

    const char* suffix = name + length - 4;
    return (suffix[0] == '.' &&
            (suffix[1] == 'p' || suffix[1] == 'P') &&
            (suffix[2] == 'n' || suffix[2] == 'N') &&
            (suffix[3] == 'g' || suffix[3] == 'G'));
}

void append_text(char* buffer, std::size_t capacity, const char* text) {
    if (!buffer || capacity == 0 || !text) {
        return;
    }

    std::size_t length = std::strlen(buffer);
    std::size_t index = 0;
    while (text[index] != '\0' && length + 1 < capacity) {
        buffer[length++] = text[index++];
    }
    buffer[length] = '\0';
}

std::uint32_t rgb_to_screen(const RGBATexel& pixel) {
    return *reinterpret_cast<const TexelWord*>(&pixel) & 0x00FFFFFFU;
}

std::uint32_t rgb_to_texel_word(std::uint32_t color) {
    return (color & 0x00FFFFFFU) | 0xFF000000U;
}

std::uint32_t texel_to_word(const RGBATexel& pixel) {
    return *reinterpret_cast<const TexelWord*>(&pixel);
}

RGBATexel texel_word_to_rgba(std::uint32_t word) {
    RGBATexel pixel = {};
    pixel.b = static_cast<std::uint8_t>(word & 0xFFU);
    pixel.g = static_cast<std::uint8_t>((word >> 8) & 0xFFU);
    pixel.r = static_cast<std::uint8_t>((word >> 16) & 0xFFU);
    pixel.a = static_cast<std::uint8_t>((word >> 24) & 0xFFU);
    return pixel;
}

void fill_texel_row(std::uint32_t* words, std::uint32_t count, const RGBATexel& color) {
    const std::uint32_t word = texel_to_word(color);
    std::uint32_t index = 0;
    const __m128i wideWord = _mm_set1_epi32(static_cast<int>(word));
    for (; index + 4 <= count; index += 4) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&words[index]), wideWord);
    }
    for (; index < count; ++index) {
        words[index] = word;
    }
}

void copy_rgb_row_to_texels(std::uint32_t* dstWords, const std::uint32_t* src, std::uint32_t count) {
    std::uint32_t index = 0;
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(0xFF000000U));
    for (; index + 4 <= count; index += 4) {
        const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&src[index]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&dstWords[index]), _mm_or_si128(pixels, alpha));
    }
    for (; index < count; ++index) {
        dstWords[index] = rgb_to_texel_word(src[index]);
    }
}

void present_texel_row_to_framebuffer(const std::uint32_t* srcWords, std::uint32_t* dst, std::uint32_t count) {
    std::uint32_t index = 0;
    const __m128i rgbMask = _mm_set1_epi32(0x00FFFFFF);
    for (; index + 4 <= count; index += 4) {
        const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srcWords[index]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&dst[index]), _mm_and_si128(pixels, rgbMask));
    }
    for (; index < count; ++index) {
        dst[index] = srcWords[index] & 0x00FFFFFFU;
    }
}

RGBATexel blend(const RGBATexel& dst, const RGBATexel& src, std::uint8_t alpha);

void clear_buffer(const RenderBuffer& buffer, const RGBATexel& color) {
    for (std::uint32_t y = 0; y < buffer.height; ++y) {
        fill_texel_row(&buffer.pixels[y * buffer.pitch], buffer.width, color);
    }
}

void draw_gradient(const RenderBuffer& buffer) {
    const RGBATexel top = rgba_from_rgb(kBackground);
    const RGBATexel bottom = rgba_from_rgb(kBackgroundBottom);
    for (std::uint32_t y = 0; y < buffer.height; ++y) {
        const std::uint32_t mix = (y * 255U) / (buffer.height > 1 ? (buffer.height - 1U) : 1U);
        RGBATexel color = {};
        color.r = static_cast<std::uint8_t>((top.r * (255U - mix) + bottom.r * mix) / 255U);
        color.g = static_cast<std::uint8_t>((top.g * (255U - mix) + bottom.g * mix) / 255U);
        color.b = static_cast<std::uint8_t>((top.b * (255U - mix) + bottom.b * mix) / 255U);
        color.a = 0xFFU;
        fill_texel_row(&buffer.pixels[y * buffer.pitch], buffer.width, color);
    }
}
