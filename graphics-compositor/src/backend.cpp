#include "backend.hpp"

namespace {

// ---------------------------------------------------------------------------
// Software backend: the historical path. Composes on the CPU and copies the
// dirty region into the memory-mapped scanout framebuffer, then fb_flush.
// ---------------------------------------------------------------------------
class SoftwareBackend final : public CompositorBackend {
public:
    const char* name() const override { return "software"; }
    CompositorBackendKind kind() const override { return CompositorBackendKind::Software; }
    bool isAccelerated() const override { return false; }

    bool probe() override { return true; }  // always available

    bool initialize(const FramebufferView& target) override {
        target_ = target;
        return target_.pixels != nullptr;
    }

    void presentFull(const ui::Canvas& scene, const CursorState& cursor,
                     const CursorImage& cursorImage) override {
        present_to_framebuffer(scene, target_);
        present_cursor_move(scene, target_, cursor, cursor, cursorImage);
    }

    void presentRect(const ui::Canvas& scene, const Rect& rect,
                     const CursorState& cursor, const CursorImage& cursorImage) override {
        present_rect_to_framebuffer(scene, target_, rect);
        present_cursor_move(scene, target_, cursor, cursor, cursorImage);
    }

    void presentCursorMove(const ui::Canvas& scene, const CursorState& oldCursor,
                           const CursorState& newCursor, const CursorImage& cursorImage) override {
        present_cursor_move(scene, target_, oldCursor, newCursor, cursorImage);
    }

private:
    FramebufferView target_ = {};
};

// ---------------------------------------------------------------------------
// VGPU backend: probes the virtio-gpu ("VGPU") device via the capset syscalls
// and is the seam where GPU-accelerated presentation will live.
//
// Status: capability-probe scaffold. Presentation still uses the software
// framebuffer path because the pieces needed to composite window surfaces on
// the GPU are not yet reachable from userland:
//   * base virtio-gpu 2D cannot host-composite multiple surfaces (one resource
//     per scanout), so real compositing needs the 3D path; and
//   * the guest-side 3D command encoders (virgl / Venus) are not exposed to
//     userland yet -- gpu_submit_3d is an opaque passthrough with no encoder.
// When an accelerated path is added, flip isAccelerated() to true and replace
// the present* bodies with GPU resource upload + submit + present.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kCapsetVirgl   = 1;   // VIRTIO_GPU_CAPSET_VIRGL
constexpr std::uint32_t kCapsetVirgl2  = 2;   // VIRTIO_GPU_CAPSET_VIRGL2
constexpr std::uint32_t kCapsetVenus   = 4;   // VIRTIO_GPU_CAPSET_VENUS
constexpr std::uint32_t kMaxCapsetProbe = 16;

class VgpuBackend final : public CompositorBackend {
public:
    const char* name() const override { return "vgpu"; }
    CompositorBackendKind kind() const override { return CompositorBackendKind::Vgpu; }
    bool isAccelerated() const override { return false; }  // scaffold: not yet accelerated

    // Enumerate virtio-gpu capsets; presence of any capset indicates a
    // 3D-capable virtio-gpu device (virgl/Venus) is available.
    bool probe() override {
        haveVirgl_ = false;
        haveVenus_ = false;
        capsetCount_ = 0;
        for (std::uint32_t index = 0; index < kMaxCapsetProbe; ++index) {
            std::GPUCapsetInfo info = {};
            info.index = index;
            if (std::gpu_capset_info(&info) == fail) {
                break;  // index exhausted or no virtio-gpu device
            }
            ++capsetCount_;
            if (info.capsetId == kCapsetVirgl || info.capsetId == kCapsetVirgl2) {
                haveVirgl_ = true;
            } else if (info.capsetId == kCapsetVenus) {
                haveVenus_ = true;
            }
        }
        return capsetCount_ > 0;
    }

    bool initialize(const FramebufferView& target) override {
        target_ = target;
        return target_.pixels != nullptr;
    }

    // Until GPU compositing lands, presentation reuses the software path so the
    // output is pixel-identical and never regresses.
    void presentFull(const ui::Canvas& scene, const CursorState& cursor,
                     const CursorImage& cursorImage) override {
        present_to_framebuffer(scene, target_);
        present_cursor_move(scene, target_, cursor, cursor, cursorImage);
    }

    void presentRect(const ui::Canvas& scene, const Rect& rect,
                     const CursorState& cursor, const CursorImage& cursorImage) override {
        present_rect_to_framebuffer(scene, target_, rect);
        present_cursor_move(scene, target_, cursor, cursor, cursorImage);
    }

    void presentCursorMove(const ui::Canvas& scene, const CursorState& oldCursor,
                           const CursorState& newCursor, const CursorImage& cursorImage) override {
        present_cursor_move(scene, target_, oldCursor, newCursor, cursorImage);
    }

    bool haveVirgl() const { return haveVirgl_; }
    bool haveVenus() const { return haveVenus_; }
    std::uint32_t capsetCount() const { return capsetCount_; }

private:
    FramebufferView target_ = {};
    bool haveVirgl_ = false;
    bool haveVenus_ = false;
    std::uint32_t capsetCount_ = 0;
};

// File-static singletons, matching the kernel backend registry's pattern.
SoftwareBackend g_software;
VgpuBackend g_vgpu;

// Match "--backend=<wanted>" on the command line.
bool arg_selects_backend(int argc, char** argv, const char* wanted) {
    if (!argv) {
        return false;
    }
    static const char kPrefix[] = "--backend=";
    const std::size_t prefixLen = sizeof(kPrefix) - 1;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }
        if (std::strncmp(arg, kPrefix, prefixLen) == 0 &&
            std::strcmp(arg + prefixLen, wanted) == 0) {
            return true;
        }
    }
    return false;
}

void log_vgpu_capabilities() {
    if (g_vgpu.capsetCount() == 0) {
        write_str("[graphics.compositor] vgpu: no virtio-gpu 3D capsets detected\n");
        return;
    }
    write_str("[graphics.compositor] vgpu: virtio-gpu present capsets=");
    write_u64(g_vgpu.capsetCount());
    write_str(g_vgpu.haveVirgl() ? " virgl=yes" : " virgl=no");
    write_str(g_vgpu.haveVenus() ? " venus=yes" : " venus=no");
    write_str(g_vgpu.isAccelerated() ? " accel=yes\n"
                                     : " accel=no (present via framebuffer)\n");
}

}  // namespace

CompositorBackend* compositor_backend_select(const FramebufferView& target, int argc, char** argv) {
    const bool forceSoftware = arg_selects_backend(argc, argv, "software");
    const bool forceVgpu = arg_selects_backend(argc, argv, "vgpu");

    const bool vgpuUsable = g_vgpu.probe();
    log_vgpu_capabilities();

    CompositorBackend* chosen = nullptr;
    if (forceSoftware) {
        chosen = &g_software;
    } else if (forceVgpu) {
        if (vgpuUsable) {
            chosen = &g_vgpu;
        } else {
            write_str("[graphics.compositor] vgpu forced but unavailable; using software\n");
            chosen = &g_software;
        }
    } else if (vgpuUsable && g_vgpu.isAccelerated()) {
        // Only auto-select vgpu once it genuinely accelerates; otherwise keep
        // the software path authoritative so there is no silent regression.
        chosen = &g_vgpu;
    } else {
        chosen = &g_software;
    }

    if (!chosen->initialize(target)) {
        write_str("[graphics.compositor] backend init failed; falling back to software\n");
        chosen = &g_software;
        chosen->initialize(target);
    }

    write_str("[graphics.compositor] backend: ");
    write_str(chosen->name());
    write_str("\n");
    return chosen;
}
