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

// ---------------------------------------------------------------------------
// Flip backend: tear-free presentation on the Intel Gen9 display (the Display
// syscall). The kernel maps two screen-sized scanout buffers; the scene is
// copied into the one not on screen and the display flips to it at the next
// vertical blank. The present* calls of one pass of the event loop only
// collect what changed; commit() waits until the back buffer has left the
// screen (at most a frame), brings it up to date and flips. So the screen
// changes at most once a frame and a burst of pointer events costs one copy.
//
// Each buffer remembers what it is missing: everything that changed since it
// was last brought up to date, and where its pointer was drawn.
// ---------------------------------------------------------------------------
class FlipBackend final : public CompositorBackend {
public:
    const char* name() const override { return emulated_ ? "flip (emulated)" : "flip"; }
    CompositorBackendKind kind() const override { return CompositorBackendKind::Flip; }
    bool isAccelerated() const override { return true; }

    bool probe() override { return acquire(false); }

    // `acceptEmulation`: without the Intel display the kernel emulates the
    // buffers (a flip copies to the framebuffer), for testing in QEMU.
    bool acquire(bool acceptEmulation) {
        std::DisplayBuffersInfo info = {};
        info.flags = acceptEmulation ? std::kDisplayEmulated : 0;
        if (std::syscall_failed(std::display(std::DisplayCommand::Acquire, reinterpret_cast<std::uint64_t>(&info)))) {
            return false;
        }
        emulated_ = (info.flags & std::kDisplayEmulated) != 0;
        if (info.count < kBuffers) {
            return false;
        }
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            buffers_[i] = { reinterpret_cast<std::uint32_t*>(info.address[i]), info.width, info.height,
                            info.pitch / 4 };
        }
        return true;
    }

    bool initialize(const FramebufferView& target) override {
        target_ = target;
        if (target_.pixels == nullptr || buffers_[0].pixels == nullptr ||
            buffers_[0].width != target_.width || buffers_[0].height != target_.height) {
            std::display(std::DisplayCommand::Release, 0);
            return false;
        }
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            missing_[i] = screen();
            cursorDrawn_[i] = {};
        }
        return true;
    }

    void presentFull(const ui::Canvas& scene, const CursorState& cursor,
                     const CursorImage& cursorImage) override {
        if (failed_) {
            present_to_framebuffer(scene, target_);
            present_cursor_move(scene, target_, cursor, cursor, cursorImage);
            return;
        }
        note(scene, screen(), cursor, cursorImage);
    }

    void presentRect(const ui::Canvas& scene, const Rect& rect,
                     const CursorState& cursor, const CursorImage& cursorImage) override {
        if (failed_) {
            present_rect_to_framebuffer(scene, target_, rect);
            present_cursor_move(scene, target_, cursor, cursor, cursorImage);
            return;
        }
        note(scene, rect, cursor, cursorImage);
    }

    void presentCursorMove(const ui::Canvas& scene, const CursorState& oldCursor,
                           const CursorState& newCursor, const CursorImage& cursorImage) override {
        if (failed_) {
            present_cursor_move(scene, target_, oldCursor, newCursor, cursorImage);
            return;
        }
        note(scene, {}, newCursor, cursorImage);
    }

    void commit() override {
        if (failed_ || !pending_) {
            return;
        }
        pending_ = false;
        const std::uint32_t back = front_ == 0 ? 1 : 0;

        // The back buffer may still be on screen until the last flip lands.
        const std::uint64_t waitStart = std::gettime();
        const bool idle = !std::syscall_failed(std::display(std::DisplayCommand::Wait, kWaitMs));
        add_elapsed(waitStart, &gTiming.flipWaitMs);
        if (!idle) {
            fallBack("the display did not take a flip");
            return;
        }

        const FramebufferView& view = buffers_[back];
        const Rect pointer = clamp_rect(cursor_rect(cursor_, *cursorImage_), view.width, view.height);
        const Rect repaint = clamp_rect(missing_[back].unite(cursorDrawn_[back]), view.width, view.height);
        if (!repaint.empty()) {
            present_rect_to_framebuffer(*scene_, view, repaint);
        }
        // The pointer over the scene (also where the repaint did not reach).
        present_cursor_move(*scene_, view, cursor_, cursor_, *cursorImage_);
        missing_[back] = {};
        cursorDrawn_[back] = pointer;

        if (std::syscall_failed(std::display(std::DisplayCommand::Flip, back))) {
            fallBack("the flip was refused");
            return;
        }
        front_ = back;
        ++gTiming.flips;
    }

private:
    static constexpr std::uint32_t kBuffers = 2;
    static constexpr std::uint64_t kWaitMs = 100;

    Rect screen() const {
        return { 0, 0, static_cast<int>(target_.width), static_cast<int>(target_.height) };
    }

    void note(const ui::Canvas& scene, const Rect& changed, const CursorState& cursor,
              const CursorImage& cursorImage) {
        scene_ = &scene;
        cursor_ = cursor;
        cursorImage_ = &cursorImage;
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            missing_[i] = missing_[i].unite(changed);
        }
        pending_ = true;
    }

    // Back to drawing straight into the firmware's framebuffer, which the
    // kernel puts back on screen: first the whole current picture.
    void fallBack(const char* why) {
        write_str("[graphics.compositor] flip backend: ");
        write_str(why);
        write_str("; presenting through the framebuffer\n");
        std::display(std::DisplayCommand::Release, 0);
        failed_ = true;
        if (scene_) {
            present_to_framebuffer(*scene_, target_);
            present_cursor_move(*scene_, target_, cursor_, cursor_, *cursorImage_);
        }
    }

    FramebufferView target_ = {};
    FramebufferView buffers_[kBuffers] = {};
    Rect missing_[kBuffers] = {};
    Rect cursorDrawn_[kBuffers] = {};
    std::uint32_t front_ = kBuffers;   // none of ours on screen yet
    const ui::Canvas* scene_ = nullptr;
    CursorState cursor_ = {};
    const CursorImage* cursorImage_ = nullptr;
    bool pending_ = false;
    bool failed_ = false;
    bool emulated_ = false;
};

// File-static singletons, matching the kernel backend registry's pattern.
SoftwareBackend g_software;
VgpuBackend g_vgpu;
FlipBackend g_flip;

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

// /display.cfg, at the root of the disk (F:\display.cfg from Windows on the
// PC; /etc is always a RAM file system), holds one word that picks the
// backend when the command line does not: "software", "flip" (also the
// kernel's emulation without the Intel display, for tests) or "auto" (the
// default: flips on the Intel display, else the framebuffer). Writing
// "software" there is the way back on a PC whose flips misbehave (an 8.3
// name: the PC's disk is FAT32).
bool config_selects(const char* wanted) {
    const std::FileHandle file = std::open("/display.cfg", O_RDONLY);
    if (file == fail) {
        return false;
    }
    char text[32] = {};
    const std::uint64_t got = std::read(file, text, sizeof(text) - 1);
    std::close(file);
    if (got == fail) {
        return false;
    }
    const std::size_t length = std::strlen(wanted);
    if (std::strncmp(text, wanted, length) != 0) {
        return false;
    }
    const char after = text[length];
    return after == '\0' || after == '\n' || after == '\r' || after == ' ';
}

}  // namespace

CompositorBackend* compositor_backend_select(const FramebufferView& target, int argc, char** argv) {
    const bool forceSoftware = arg_selects_backend(argc, argv, "software") || config_selects("software");
    const bool forceVgpu = arg_selects_backend(argc, argv, "vgpu");
    const bool forceFlip = arg_selects_backend(argc, argv, "flip") || config_selects("flip");

    const bool vgpuUsable = g_vgpu.probe();
    log_vgpu_capabilities();

    CompositorBackend* chosen = nullptr;
#ifdef INSTANTOS_COMPOSITOR_GL
    // The GPU draws the scene: on the Intel display by itself, with the
    // kernel's emulation (QEMU, softpipe) only when asked ("gl").
    const bool forceGl = arg_selects_backend(argc, argv, "gl") || config_selects("gl");
    CompositorBackend* gl = nullptr;
    if (!forceSoftware && !forceVgpu && !forceFlip) {
        gl = gl_backend_start(forceGl);
        if (!gl && forceGl) {
            write_str("[graphics.compositor] gl forced but unavailable\n");
        }
    }
#else
    CompositorBackend* gl = nullptr;
#endif
    if (gl) {
        chosen = gl;
    } else if (forceSoftware) {
        chosen = &g_software;
    } else if (!forceVgpu && g_flip.acquire(forceFlip)) {
        // The Intel Gen9 display: tear-free flips between two buffers.
        chosen = &g_flip;
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
