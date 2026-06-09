#include "common.hpp"

#define STBI_ASSERT(x) ((void)0)
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>

void draw_gradient_pixels(std::uint32_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) {
    if (!pixels || width == 0 || height == 0) {
        return;
    }

    const RenderBuffer buffer = { pixels, width, height, pitch };
    draw_gradient(buffer);
}

bool ensure_background_storage(DesktopBackground* background, std::uint32_t width, std::uint32_t height) {
    if (!background || width == 0 || height == 0) {
        return false;
    }

    if (background->pixels && background->width == width && background->height == height) {
        return true;
    }

    delete[] background->pixels;
    background->pixels = new (std::nothrow) std::uint32_t[static_cast<std::size_t>(width) * static_cast<std::size_t>(height)];
    if (!background->pixels) {
        background->width = 0;
        background->height = 0;
        background->ready = false;
        background->currentPath[0] = '\0';
        return false;
    }

    background->width = width;
    background->height = height;
    background->ready = false;
    background->currentPath[0] = '\0';
    return true;
}

void scale_background_image(DesktopBackground* background, int imageWidth, int imageHeight, const unsigned char* imagePixels) {
    if (!background || !background->pixels || !imagePixels || imageWidth <= 0 || imageHeight <= 0) {
        return;
    }

    draw_gradient_pixels(background->pixels, background->width, background->height, background->width);

    std::uint32_t drawWidth = background->width;
    std::uint32_t drawHeight = static_cast<std::uint32_t>((static_cast<std::uint64_t>(imageHeight) * background->width) / imageWidth);
    if (drawHeight > background->height) {
        drawHeight = background->height;
        drawWidth = static_cast<std::uint32_t>((static_cast<std::uint64_t>(imageWidth) * background->height) / imageHeight);
    }
    if (drawWidth == 0 || drawHeight == 0) {
        return;
    }

    const int offsetX = static_cast<int>((background->width - drawWidth) / 2U);
    const int offsetY = static_cast<int>((background->height - drawHeight) / 2U);
    for (std::uint32_t y = 0; y < drawHeight; ++y) {
        const int srcY = static_cast<int>((static_cast<std::uint64_t>(y) * imageHeight) / drawHeight);
        for (std::uint32_t x = 0; x < drawWidth; ++x) {
            const int srcX = static_cast<int>((static_cast<std::uint64_t>(x) * imageWidth) / drawWidth);
            const unsigned char* src = imagePixels + ((srcY * imageWidth + srcX) * 4);
            RGBATexel pixel = {};
            pixel.r = src[0];
            pixel.g = src[1];
            pixel.b = src[2];
            pixel.a = 0xFFU;
            std::uint32_t& dst = background->pixels[(offsetY + static_cast<int>(y)) * background->width + offsetX + static_cast<int>(x)];
            dst = src[3] == 255 ? texel_to_word(pixel) : texel_to_word(blend(texel_word_to_rgba(dst), pixel, src[3]));
        }
    }
}

bool load_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height, const char* path) {
    if (!background || !path || path[0] == '\0' || !ensure_background_storage(background, width, height)) {
        return false;
    }

    std::Stat stat = {};
    if (std::stat(path, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
        return false;
    }

    const std::size_t fileSize = static_cast<std::size_t>(stat.st_size);
    unsigned char* encoded = new (std::nothrow) unsigned char[fileSize];
    if (!encoded) {
        std::close(file);
        return false;
    }

    std::size_t totalRead = 0;
    while (totalRead < fileSize) {
        const std::uint64_t bytesRead = std::read(file, encoded + totalRead, fileSize - totalRead);
        if (bytesRead == fail || bytesRead == 0) {
            delete[] encoded;
            std::close(file);
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);

    int imageWidth = 0;
    int imageHeight = 0;
    int components = 0;
    unsigned char* imagePixels = stbi_load_from_memory(encoded, static_cast<int>(fileSize), &imageWidth, &imageHeight, &components, 4);
    delete[] encoded;
    if (!imagePixels || imageWidth <= 0 || imageHeight <= 0) {
        return false;
    }

    scale_background_image(background, imageWidth, imageHeight, imagePixels);
    stbi_image_free(imagePixels);

    std::strncpy(background->currentPath, path, sizeof(background->currentPath) - 1);
    background->currentPath[sizeof(background->currentPath) - 1] = '\0';
    background->ready = true;
    return true;
}

bool load_first_available_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height) {
    if (load_background(background, width, height, kDefaultBackground)) {
        return true;
    }

    std::DirEntry entries[32] = {};
    const std::uint64_t found = std::readdir(kBackgroundDirectory, entries, 32);
    if (found == fail) {
        return false;
    }

    for (std::uint64_t index = 0; index < found; ++index) {
        if (entries[index].type != std::FileType::Regular || !has_png_suffix(entries[index].name)) {
            continue;
        }

        char path[192] = {};
        append_text(path, sizeof(path), kBackgroundDirectory);
        append_text(path, sizeof(path), "/");
        append_text(path, sizeof(path), entries[index].name);
        if (load_background(background, width, height, path)) {
            return true;
        }
    }

    return false;
}

void draw_background(const RenderBuffer& buffer, const DesktopBackground* background) {
    if (!background || !background->pixels || !background->ready ||
        background->width != buffer.width || background->height != buffer.height) {
        draw_gradient(buffer);
        return;
    }

    for (std::uint32_t y = 0; y < buffer.height; ++y) {
        std::memcpy(
            &buffer.pixels[y * buffer.pitch],
            &background->pixels[y * background->width],
            sizeof(RGBATexel) * buffer.width
        );
    }
}

void draw_background_rect(const RenderBuffer& buffer, const DesktopBackground* background, const Rect& rect) {
    int startX = rect.x < 0 ? 0 : rect.x;
    int startY = rect.y < 0 ? 0 : rect.y;
    int endX = rect.x + rect.width;
    int endY = rect.y + rect.height;
    if (endX > static_cast<int>(buffer.width)) {
        endX = static_cast<int>(buffer.width);
    }
    if (endY > static_cast<int>(buffer.height)) {
        endY = static_cast<int>(buffer.height);
    }
    if (startX >= endX || startY >= endY) {
        return;
    }

    if (!background || !background->pixels || !background->ready ||
        background->width != buffer.width || background->height != buffer.height) {
        for (int drawYInt = startY; drawYInt < endY; ++drawYInt) {
            const std::uint32_t drawY = static_cast<std::uint32_t>(drawYInt);
            const RGBATexel top = rgba_from_rgb(kBackground);
            const RGBATexel bottom = rgba_from_rgb(kBackgroundBottom);
            const std::uint32_t mix = (drawY * 255U) / (buffer.height > 1 ? (buffer.height - 1U) : 1U);
            RGBATexel color = {};
            color.r = static_cast<std::uint8_t>((top.r * (255U - mix) + bottom.r * mix) / 255U);
            color.g = static_cast<std::uint8_t>((top.g * (255U - mix) + bottom.g * mix) / 255U);
            color.b = static_cast<std::uint8_t>((top.b * (255U - mix) + bottom.b * mix) / 255U);
            color.a = 0xFFU;
            fill_texel_row(&buffer.pixels[drawY * buffer.pitch + static_cast<std::uint32_t>(startX)],
                           static_cast<std::uint32_t>(endX - startX), color);
        }
        return;
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        std::memcpy(
            &buffer.pixels[drawY * buffer.pitch + static_cast<std::uint32_t>(startX)],
            &background->pixels[drawY * background->width + static_cast<std::uint32_t>(startX)],
            sizeof(std::uint32_t) * static_cast<std::uint32_t>(endX - startX)
        );
    }
}
