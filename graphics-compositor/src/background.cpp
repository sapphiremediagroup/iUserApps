#include "common.hpp"

namespace {

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

// The image fitted into the screen (nearest pixel, aspect kept) over the
// default gradient, which shows in the bars it leaves.
void scale_background_image(DesktopBackground* background, const ui::Image& image) {
    ui::Canvas canvas(background->pixels, static_cast<int>(background->width),
                      static_cast<int>(background->height), static_cast<int>(background->width));
    canvas.gradient({ 0, 0, canvas.width(), canvas.height() }, ui::rgb(kBackground), ui::rgb(kBackgroundBottom));

    const int imageWidth = image.width();
    const int imageHeight = image.height();
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
            ui::Color pixel = image.pixels()[srcY * imageWidth + srcX];
            const std::uint8_t alpha = pixel.a;
            pixel.a = 0xFF;
            std::uint32_t& dst = background->pixels[(offsetY + static_cast<int>(y)) * background->width + offsetX + static_cast<int>(x)];
            dst = alpha == 255 ? pixel.word() : ui::blend(ui::Color::from_word(dst), pixel, alpha).word();
        }
    }
}

}  // namespace

bool load_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height, const char* path) {
    if (!background || !path || path[0] == '\0' || !ensure_background_storage(background, width, height)) {
        return false;
    }

    ui::Image image;
    if (!image.load(path)) {
        return false;
    }
    scale_background_image(background, image);

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

// The wallpaper, or the default gradient without one, over the canvas's clip.
void draw_background(ui::Canvas& canvas, const DesktopBackground* background) {
    const Rect screen = { 0, 0, canvas.width(), canvas.height() };
    if (!background || !background->pixels || !background->ready ||
        background->width != static_cast<std::uint32_t>(canvas.width()) ||
        background->height != static_cast<std::uint32_t>(canvas.height())) {
        canvas.gradient(screen, ui::rgb(kBackground), ui::rgb(kBackgroundBottom));
        return;
    }
    canvas.copy(background->pixels, static_cast<int>(background->width), screen, { 0, 0 });
}
