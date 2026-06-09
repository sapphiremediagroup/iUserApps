#include "common.hpp"

void present_to_framebuffer(const RenderBuffer& source, const FramebufferView& target) {
    const std::uint64_t presentStart = std::gettime();
    const std::uint32_t copyWidth = source.width < target.width ? source.width : target.width;
    const std::uint32_t copyHeight = source.height < target.height ? source.height : target.height;

    for (std::uint32_t y = 0; y < copyHeight; ++y) {
        present_texel_row_to_framebuffer(
            &source.pixels[y * source.pitch],
            &target.pixels[y * target.pitch],
            copyWidth
        );
    }

    std::fb_flush(0, 0, copyWidth, copyHeight);
    add_elapsed(presentStart, &gTiming.presentMs);
}

TripleBufferState::TripleBufferState()
    : buffers{},
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
        buffers[i].pixels = new (std::nothrow) std::uint32_t[pixelCount];
        if (buffers[i].pixels == nullptr) {
            return false;
        }
        buffers[i].width = width;
        buffers[i].height = height;
        buffers[i].pitch = width;
        clear_buffer(buffers[i], rgba_from_rgb(kBackground));
    }

    nextIndex = 0;
    return true;
}

RenderBuffer& TripleBufferState::acquire() {
    RenderBuffer& buffer = buffers[nextIndex];
    nextIndex = (nextIndex + 1U) % kCompositorBufferCount;
    return buffer;
}

void TripleBufferState::reset() {
    for (std::uint32_t i = 0; i < kCompositorBufferCount; ++i) {
        delete[] buffers[i].pixels;
        buffers[i].pixels = nullptr;
        buffers[i].width = 0;
        buffers[i].height = 0;
        buffers[i].pitch = 0;
    }
    nextIndex = 0;
}
