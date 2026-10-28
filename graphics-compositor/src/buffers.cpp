#include "common.hpp"

void present_to_framebuffer(const ui::Canvas& source, const FramebufferView& target) {
    const std::uint64_t presentStart = std::gettime();
    const std::uint32_t sourceWidth = static_cast<std::uint32_t>(source.width());
    const std::uint32_t sourceHeight = static_cast<std::uint32_t>(source.height());
    const std::uint32_t copyWidth = sourceWidth < target.width ? sourceWidth : target.width;
    const std::uint32_t copyHeight = sourceHeight < target.height ? sourceHeight : target.height;

    for (std::uint32_t y = 0; y < copyHeight; ++y) {
        present_texel_row_to_framebuffer(
            &source.pixels()[y * source.pitch()],
            &target.pixels[y * target.pitch],
            copyWidth
        );
    }

    std::fb_flush(0, 0, copyWidth, copyHeight);
    add_elapsed(presentStart, &gTiming.presentMs);
}

TripleBufferState::TripleBufferState()
    : pixels{},
      buffers{},
      nextIndex(0) {
}

TripleBufferState::~TripleBufferState() {
    reset();
}

bool TripleBufferState::initialize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }

    reset();
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::uint32_t i = 0; i < kCompositorBufferCount; ++i) {
        pixels[i] = new (std::nothrow) std::uint32_t[pixelCount];
        if (pixels[i] == nullptr) {
            return false;
        }
        buffers[i] = ui::Canvas(pixels[i], static_cast<int>(width), static_cast<int>(height), static_cast<int>(width));
        buffers[i].clear(ui::rgb(kBackground));
    }

    nextIndex = 0;
    return true;
}

ui::Canvas& TripleBufferState::acquire() {
    ui::Canvas& buffer = buffers[nextIndex];
    nextIndex = (nextIndex + 1U) % kCompositorBufferCount;
    return buffer;
}

void TripleBufferState::reset() {
    for (std::uint32_t i = 0; i < kCompositorBufferCount; ++i) {
        delete[] pixels[i];
        pixels[i] = nullptr;
        buffers[i] = ui::Canvas();
    }
    nextIndex = 0;
}
