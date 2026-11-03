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

// Three box blurs (close to a Gaussian) of the wallpaper under the taskbar,
// kept so the bar can show it without blurring on every redraw.
void blur_bar_backdrop(DesktopBackground* background) {
    const Rect bar = taskbar_bar_rect(background->width, background->height);
    const int reach = 3 * kBarBlurRadius;
    const Rect source = Rect { bar.x - reach, bar.y - reach, bar.width + 2 * reach, bar.height + 2 * reach }
        .intersect({ 0, 0, static_cast<int>(background->width), static_cast<int>(background->height) });
    delete[] background->barBlur;
    background->barBlur = nullptr;
    if (source.empty() || bar.empty()) {
        return;
    }
    const int w = source.width;
    const int h = source.height;
    std::uint32_t* work = new (std::nothrow) std::uint32_t[static_cast<std::size_t>(w) * h];
    std::uint32_t* temp = new (std::nothrow) std::uint32_t[static_cast<std::size_t>(w) * h];
    std::uint32_t* out = new (std::nothrow) std::uint32_t[static_cast<std::size_t>(bar.width) * bar.height];
    if (!work || !temp || !out) {
        delete[] work;
        delete[] temp;
        delete[] out;
        return;
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(&work[y * w], &background->pixels[(source.y + y) * background->width + source.x],
                    static_cast<std::size_t>(w) * 4);
    }
    // One pass along rows (step 1) or columns (step w) of `count` lines.
    auto box = [&](const std::uint32_t* from, std::uint32_t* to, int length, int lines, int step, int lineStep) {
        const int r = kBarBlurRadius;
        for (int line = 0; line < lines; ++line) {
            const std::uint32_t* in = from + line * lineStep;
            std::uint32_t* res = to + line * lineStep;
            int sum[3] = { 0, 0, 0 };
            for (int k = -r; k <= r; ++k) {
                const int at = k < 0 ? 0 : k >= length ? length - 1 : k;
                const std::uint32_t p = in[at * step];
                sum[0] += p & 0xFF;
                sum[1] += (p >> 8) & 0xFF;
                sum[2] += (p >> 16) & 0xFF;
            }
            const int span = 2 * r + 1;
            for (int i = 0; i < length; ++i) {
                res[i * step] = 0xFF000000u | static_cast<std::uint32_t>(sum[0] / span) |
                                (static_cast<std::uint32_t>(sum[1] / span) << 8) |
                                (static_cast<std::uint32_t>(sum[2] / span) << 16);
                const int addAt = i + r + 1 >= length ? length - 1 : i + r + 1;
                const int dropAt = i - r < 0 ? 0 : i - r;
                const std::uint32_t add = in[addAt * step];
                const std::uint32_t drop = in[dropAt * step];
                sum[0] += static_cast<int>(add & 0xFF) - static_cast<int>(drop & 0xFF);
                sum[1] += static_cast<int>((add >> 8) & 0xFF) - static_cast<int>((drop >> 8) & 0xFF);
                sum[2] += static_cast<int>((add >> 16) & 0xFF) - static_cast<int>((drop >> 16) & 0xFF);
            }
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        box(work, temp, w, h, 1, w);
        box(temp, work, h, w, w, 1);
    }
    for (int y = 0; y < bar.height; ++y) {
        std::memcpy(&out[y * bar.width], &work[(bar.y - source.y + y) * w + (bar.x - source.x)],
                    static_cast<std::size_t>(bar.width) * 4);
    }
    delete[] work;
    delete[] temp;
    background->barBlur = out;
    background->barBlurRect = bar;
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
    blur_bar_backdrop(background);
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
