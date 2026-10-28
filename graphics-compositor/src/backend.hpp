#pragma once

#include "common.hpp"

enum class CompositorBackendKind : std::uint32_t {
    Software,
    Vgpu
};

class CompositorBackend {
public:
    virtual ~CompositorBackend() = default;

    virtual const char* name() const = 0;
    virtual CompositorBackendKind kind() const = 0;

    virtual bool isAccelerated() const = 0;

    virtual bool probe() = 0;
    virtual bool initialize(const FramebufferView& target) = 0;

    virtual void presentFull(const ui::Canvas& scene,
                             const CursorState& cursor,
                             const CursorImage& cursorImage) = 0;
    virtual void presentRect(const ui::Canvas& scene, const Rect& rect,
                             const CursorState& cursor,
                             const CursorImage& cursorImage) = 0;
    virtual void presentCursorMove(const ui::Canvas& scene,
                                   const CursorState& oldCursor,
                                   const CursorState& newCursor,
                                   const CursorImage& cursorImage) = 0;
};

CompositorBackend* compositor_backend_select(const FramebufferView& target, int argc, char** argv);
