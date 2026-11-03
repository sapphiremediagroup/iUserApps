#pragma once

#include "common.hpp"

enum class CompositorBackendKind : std::uint32_t {
    Software,
    Vgpu,
    Flip,
    Gl
};

class CompositorBackend {
public:
    virtual ~CompositorBackend() = default;

    virtual const char* name() const = 0;
    virtual CompositorBackendKind kind() const = 0;

    virtual bool isAccelerated() const = 0;
    // It draws the scene itself (the GL backend, from setScene's sources):
    // the CPU's redraw_scene is skipped and present* only mark a frame.
    virtual bool drawsScene() const { return false; }

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

    // The end of one pass of the event loop. A backend that collects the
    // present* calls (the flip backend) shows them now; the others already
    // did.
    virtual void commit() {}
};

CompositorBackend* compositor_backend_select(const FramebufferView& target, int argc, char** argv);

#ifdef INSTANTOS_COMPOSITOR_GL
// gl_backend.cpp. `acceptEmulation` also takes the kernel's emulated display
// (QEMU: frames read back); otherwise only the Intel display's dma-bufs.
CompositorBackend* gl_backend_start(bool acceptEmulation);
void gl_backend_set_scene(const std::WindowInfo* windows, const std::uint64_t* count, SurfaceCacheEntry* cache,
                          const DesktopBackground* background, const TaskbarAssets* taskbar,
                          const WindowControlAssets* controls);
#endif
