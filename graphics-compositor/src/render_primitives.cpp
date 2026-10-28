#include "common.hpp"

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

// Scene pixels (B,G,R,A) to the framebuffer, which has no alpha byte.
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
