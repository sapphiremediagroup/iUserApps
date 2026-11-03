// The GL backend (step 8c): the whole scene drawn with OpenGL ES 3 through
// Mesa, every frame, into the display's scanout buffers, then flipped as the
// flip backend does. Only the mlibc build has it (INSTANTOS_COMPOSITOR_GL, set
// by tools/build-compositor-mlibc.sh when Mesa's headers are there); EGL and
// GLES are opened with dlopen, so the compositor still starts without Mesa.
//
// What the GPU draws itself: the wallpaper (a texture), each window's shadow
// (the CPU path's falloff, maxAlpha * t^2 with t = 1 - d / kShadowBlur from
// the frame moved down kShadowOffsetY, as a distance field), its rounded body
// and 1 px border, its contents and the pointer. Its contents are a texture:
// a GPU-buffer surface's dma-buf imported as is (EGL_EXT_image_dma_buf_import,
// no copy), a shared-memory surface uploaded when it was committed again. What
// the CPU still draws, into a screen-sized side buffer, is only ever opaque:
// each window's title bar (icon, name, controls; draw_window_title), the
// taskbar (draw_taskbar over its blurred backdrop) and the tooltip; they go
// up as textures whose rounded corners the GPU cuts. ui::Canvas blends colour
// onto an opaque picture and keeps no alpha, so nothing it draws needs any.
//
// The target: on the Intel display both scanout buffers come as dma-bufs
// (Display ExportBuffer, uncached: the display does not snoop), imported as
// renderbuffers, so the GPU draws into what is scanned out. Without it (the
// kernel's emulation in QEMU, with softpipe) the frame is drawn into a
// renderbuffer of its own and read back into the mapped buffer, swizzled to
// BGRA by the shader so the rows copy as they are.
#ifdef INSTANTOS_COMPOSITOR_GL

#include "backend.hpp"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <instantos/window.h>

// EGL's native types are the generic Unix ones (pointers), as for Mesa's
// own InstantOS build; no X11.
#ifndef __unix__
#define __unix__ 1
#endif
#define EGL_NO_X11 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

namespace {

// DRM fourcc codes (drm_fourcc.h): XRGB8888 is B, G, R, X in memory.
constexpr EGLint kFourccXrgb8888 = 0x34325258;   // 'XR24'

// The entry points, from libEGL.so.1 and libGLESv2.so.2.
#define GL_FUNCTIONS(X)                                                                                         \
    X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog)                \
    X(glCreateProgram) X(glAttachShader) X(glBindAttribLocation) X(glLinkProgram) X(glGetProgramiv)              \
    X(glGetProgramInfoLog) X(glUseProgram) X(glGetUniformLocation) X(glUniform4f) X(glUniform2f) X(glUniform1i)  \
    X(glUniform1f) X(glGenTextures) X(glBindTexture) X(glTexParameteri) X(glTexImage2D) X(glTexSubImage2D)       \
    X(glPixelStorei) X(glDeleteTextures) X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferRenderbuffer) \
    X(glGenRenderbuffers) X(glBindRenderbuffer) X(glRenderbufferStorage) X(glCheckFramebufferStatus)            \
    X(glViewport) X(glClearColor) X(glClear) X(glEnable) X(glDisable) X(glBlendFunc) X(glVertexAttribPointer)   \
    X(glEnableVertexAttribArray) X(glDrawArrays) X(glFinish) X(glReadPixels) X(glGetError) X(glGetString)       \
    X(glActiveTexture) X(glGenBuffers) X(glBindBuffer) X(glBufferData)
#define EGL_FUNCTIONS(X)                                                                                        \
    X(eglGetDisplay) X(eglInitialize) X(eglBindAPI) X(eglChooseConfig) X(eglCreateContext) X(eglMakeCurrent)    \
    X(eglGetProcAddress) X(eglQueryString) X(eglGetError)
#define DECLARE(name) decltype(&::name) p_##name = nullptr;
GL_FUNCTIONS(DECLARE)
EGL_FUNCTIONS(DECLARE)
PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES = nullptr;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC p_glEGLImageTargetRenderbufferStorageOES = nullptr;

bool load_functions() {
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void* gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gles) {
        write_str("[graphics.compositor] gl: Mesa's libEGL/libGLESv2 are not there\n");
        return false;
    }
    bool ok = true;
#define LOAD_EGL(name) ok = ok && (p_##name = reinterpret_cast<decltype(p_##name)>(dlsym(egl, #name))) != nullptr;
#define LOAD_GL(name) ok = ok && (p_##name = reinterpret_cast<decltype(p_##name)>(dlsym(gles, #name))) != nullptr;
    EGL_FUNCTIONS(LOAD_EGL)
    GL_FUNCTIONS(LOAD_GL)
    if (!ok) {
        write_str("[graphics.compositor] gl: an entry point is missing\n");
        return false;
    }
    p_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(p_eglGetProcAddress("eglCreateImageKHR"));
    p_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(p_eglGetProcAddress("eglDestroyImageKHR"));
    p_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(p_eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    p_glEGLImageTargetRenderbufferStorageOES = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        p_eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    return true;
}

const char* kVertexShader =
    "#version 300 es\n"
    "in vec2 corner;\n"
    "uniform vec4 u_quad;\n"     // x, y, width, height in screen pixels
    "uniform vec4 u_uv;\n"       // u0, v0, u1, v1
    "uniform vec2 u_screen;\n"
    "out vec2 v_pixel;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = u_quad.xy + corner * u_quad.zw;\n"
    "    v_pixel = p;\n"
    "    v_uv = mix(u_uv.xy, u_uv.zw, corner);\n"
    // Screen row 0 is the target's first row in memory (GL's y = 0).
    "    gl_Position = vec4(p / u_screen * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kFragmentShader =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_pixel;\n"
    "in vec2 v_uv;\n"
    "uniform int u_mode;\n"       // 0 colour, 1 opaque texture, 2 straight-alpha texture, 3 shadow
    "uniform vec4 u_color;\n"     // RGBA (colour; for textures only .a, the opacity)
    "uniform sampler2D u_tex;\n"
    "uniform int u_bgra;\n"       // the texture holds B, G, R, A in its r, g, b, a
    "uniform int u_outBgra;\n"    // write B, G, R, A (the read-back target)
    "uniform vec4 u_shape;\n"     // the rounded rectangle that cuts it: x, y, width, height
    "uniform vec4 u_radii;\n"     // top left, top right, bottom right, bottom left
    "uniform float u_ring;\n"     // > 0: only a band this wide inside the shape
    "uniform vec2 u_shadow;\n"    // max alpha (0..1), reach
    "out vec4 fragColor;\n"
    "float rounded(vec2 p, vec4 rect, vec4 radii, float inset) {\n"
    "    vec2 half_ = rect.zw * 0.5 - inset;\n"
    "    vec2 q = p - (rect.xy + rect.zw * 0.5);\n"
    "    float r = q.x > 0.0 ? (q.y > 0.0 ? radii.z : radii.y) : (q.y > 0.0 ? radii.w : radii.x);\n"
    "    r = max(r - inset, 0.0);\n"
    "    vec2 d = abs(q) - half_ + r;\n"
    "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "    vec4 colour;\n"
    "    if (u_mode == 3) {\n"
    "        float d = rounded(v_pixel, u_shape, u_radii, 0.0);\n"
    "        float t = d <= 0.0 ? 1.0 : clamp(1.0 - d / u_shadow.y, 0.0, 1.0);\n"
    "        colour = vec4(0.0, 0.0, 0.0, u_shadow.x * t * t);\n"
    "    } else {\n"
    "        float cover = clamp(0.5 - rounded(v_pixel, u_shape, u_radii, 0.0), 0.0, 1.0);\n"
    "        if (u_ring > 0.0) {\n"
    "            cover -= clamp(0.5 - rounded(v_pixel, u_shape, u_radii, u_ring), 0.0, 1.0);\n"
    "        }\n"
    "        if (u_mode == 0) {\n"
    "            float a = u_color.a * cover;\n"
    "            colour = vec4(u_color.rgb * a, a);\n"
    "        } else {\n"
    "            vec4 texel = texture(u_tex, v_uv);\n"
    "            if (u_bgra != 0) texel = texel.bgra;\n"
    "            float a = (u_mode == 2 ? texel.a : 1.0) * u_color.a * cover;\n"
    "            colour = vec4(texel.rgb * a, a);\n"
    "        }\n"
    "    }\n"
    "    fragColor = u_outBgra != 0 ? colour.bgra : colour;\n"
    "}\n";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = p_glCreateShader(type);
    p_glShaderSource(shader, 1, &source, nullptr);
    p_glCompileShader(shader);
    GLint ok = 0;
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        p_glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        write_str("[graphics.compositor] gl: shader: ");
        write_str(log);
        write_str("\n");
        return 0;
    }
    return shader;
}

struct Uniforms {
    GLint quad, uv, screen, mode, color, tex, bgra, outBgra, shape, radii, ring, shadow;
};

// One texture of the CPU's drawing or a window's pixels.
struct Texture {
    GLuint name = 0;
    int width = 0;
    int height = 0;
};

// A window's contents on the GPU.
struct SurfaceTexture {
    std::uint64_t id = 0;
    Texture texture;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;   // a GPU-buffer surface's dma-buf
    bool shared = false;                     // shared memory: uploaded on each commit
    std::uint64_t uploaded = ~0ULL;          // the generation in the texture
    bool seen = false;
    std::uint64_t lastSeen = 0;              // the frame it was last drawn in
};

constexpr std::uint64_t kTextureKeepFrames = 240;

struct TitleTexture {
    std::uint64_t windowId = 0;
    Texture texture;
    std::uint64_t key = 0;   // the Fingerprint of what it shows
    bool seen = false;
};

// What a CPU-drawn texture shows, so it is drawn again only when that
// changes (FNV-1a).
struct Fingerprint {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    Fingerprint& add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value = (value ^ bytes[i]) * 0x100000001b3ULL;
        }
        return *this;
    }
    Fingerprint& add(std::int64_t number) { return add(&number, sizeof(number)); }
};

constexpr int kMaxTextures = 64;

class GlBackend final : public CompositorBackend {
public:
    const char* name() const override { return readBack_ ? "gl (read back)" : "gl"; }
    CompositorBackendKind kind() const override { return CompositorBackendKind::Gl; }
    bool isAccelerated() const override { return true; }
    bool drawsScene() const override { return true; }
    bool probe() override { return false; }

    // The display, EGL, the program and the targets. `acceptEmulation` takes
    // the kernel's emulated buffers (QEMU), drawn by reading back.
    bool start(bool acceptEmulation) {
        std::DisplayBuffersInfo info = {};
        info.flags = acceptEmulation ? std::kDisplayEmulated : 0;
        if (std::syscall_failed(std::display(std::DisplayCommand::Acquire, reinterpret_cast<std::uint64_t>(&info))) ||
            info.count < kBuffers) {
            return false;
        }
        width_ = static_cast<int>(info.width);
        height_ = static_cast<int>(info.height);
        pitch_ = info.pitch;
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            mapped_[i] = reinterpret_cast<std::uint32_t*>(info.address[i]);
        }
        // The scanout buffers as dma-bufs; the emulation has none.
        long fds[kBuffers] = {-1, -1};
        bool exported = true;
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            const std::uint64_t handle = std::display(std::DisplayCommand::ExportBuffer, i);
            if (std::syscall_failed(handle)) {
                exported = false;
                break;
            }
            fds[i] = instantos_handle_fd(static_cast<long>(handle));
        }
        if (!exported && !acceptEmulation) {
            std::display(std::DisplayCommand::Release, 0);
            return false;
        }
        readBack_ = !exported;
        if (!load_functions() || !startEgl() || !startProgram() || !startTargets(fds)) {
            for (long fd : fds) {
                if (fd >= 0) close(static_cast<int>(fd));
            }
            std::display(std::DisplayCommand::Release, 0);
            return false;
        }
        for (long fd : fds) {
            if (fd >= 0) close(static_cast<int>(fd));   // the images hold the buffers
        }
        side_ = new (std::nothrow) std::uint32_t[static_cast<std::size_t>(width_) * height_];
        if (!side_) {
            std::display(std::DisplayCommand::Release, 0);
            return false;
        }
        write_str("[graphics.compositor] gl: ");
        write_str(reinterpret_cast<const char*>(p_glGetString(GL_RENDERER)));
        write_str(readBack_ ? ", frames read back into the emulated buffers\n" : ", drawing into the scanout buffers\n");
        return true;
    }

    void setScene(const std::WindowInfo* windows, const std::uint64_t* count, SurfaceCacheEntry* cache,
                  const DesktopBackground* background, const TaskbarAssets* taskbar,
                  const WindowControlAssets* controls) {
        windows_ = windows;
        windowCount_ = count;
        cache_ = cache;
        background_ = background;
        taskbar_ = taskbar;
        controls_ = controls;
    }

    bool initialize(const FramebufferView& target) override {
        target_ = target;
        return static_cast<std::uint32_t>(width_) == target.width && static_cast<std::uint32_t>(height_) == target.height;
    }

    void presentFull(const ui::Canvas&, const CursorState& cursor, const CursorImage& image) override {
        note(cursor, image);
    }
    void presentRect(const ui::Canvas&, const Rect&, const CursorState& cursor, const CursorImage& image) override {
        note(cursor, image);
    }
    void presentCursorMove(const ui::Canvas&, const CursorState&, const CursorState& cursor,
                           const CursorImage& image) override {
        note(cursor, image);
    }

    void commit() override {
        if (!pending_ || !windows_) {
            return;
        }
        pending_ = false;
        const std::uint32_t back = front_ == 0 ? 1 : 0;
        const std::uint64_t waitStart = std::gettime();
        std::display(std::DisplayCommand::Wait, kWaitMs);   // the back buffer may still be on screen
        add_elapsed(waitStart, &gTiming.flipWaitMs);

        const std::uint64_t drawStart = std::gettime();
        p_glBindFramebuffer(GL_FRAMEBUFFER, readBack_ ? framebuffers_[0] : framebuffers_[back]);
        p_glViewport(0, 0, width_, height_);
        drawFrame();
        const std::uint64_t recorded = std::gettime();
        p_glFinish();
        if (readBack_) {
            readBack(back);
        }
        const std::uint64_t finished = std::gettime();
        add_elapsed(drawStart, &gTiming.sceneRedrawMs);
        // Where a frame's time goes: recording it (the CPU's drawing and
        // uploads included) and the GPU's work until it is done.
        stats_.recordMs += recorded - drawStart;
        stats_.finishMs += finished - recorded;
        if (++stats_.frames == 120) {
            write_str("[graphics.compositor] gl: 120 frames: recording ");
            write_u64(stats_.recordMs);
            write_str(" ms, GPU until done ");
            write_u64(stats_.finishMs);
            write_str(" ms; imports ");
            write_u64(stats_.imports);
            write_str(", uploads: windows ");
            write_u64(stats_.windowUploads);
            write_str(" titles ");
            write_u64(stats_.titleUploads);
            write_str(" taskbar ");
            write_u64(stats_.taskbarUploads);
            write_str("\n");
            stats_ = {};
        }
        if (!std::syscall_failed(std::display(std::DisplayCommand::Flip, back))) {
            front_ = back;
            ++gTiming.flips;
        }
    }

private:
    static constexpr std::uint32_t kBuffers = 2;
    static constexpr std::uint64_t kWaitMs = 100;

    bool startEgl() {
        // Read back (the emulated display, QEMU): no GPU behind the render
        // node, so Mesa's CPU renderer on the surfaceless platform, as
        // `gl-offscreen software` does.
        EGLDisplay display = EGL_NO_DISPLAY;
        if (readBack_) {
            setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            setenv("GALLIUM_DRIVER", "softpipe", 1);
            auto getPlatformDisplay =
                reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(p_eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (getPlatformDisplay) {
                display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            }
        } else {
            display = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
        EGLint major = 0, minor = 0;
        if (display == EGL_NO_DISPLAY || !p_eglInitialize(display, &major, &minor)) {
            write_str("[graphics.compositor] gl: eglInitialize failed\n");
            return false;
        }
        const char* extensions = p_eglQueryString(display, EGL_EXTENSIONS);
        if (!extensions || !std::strstr(extensions, "EGL_KHR_surfaceless_context") ||
            (!readBack_ && !std::strstr(extensions, "EGL_EXT_image_dma_buf_import"))) {
            write_str("[graphics.compositor] gl: EGL lacks surfaceless contexts or dma-buf import\n");
            return false;
        }
        dmaBufImport_ = std::strstr(extensions, "EGL_EXT_image_dma_buf_import") != nullptr && p_eglCreateImageKHR &&
                        p_glEGLImageTargetTexture2DOES;
        p_eglBindAPI(EGL_OPENGL_ES_API);
        // No surface is ever made (the targets are framebuffer objects), so
        // any surface type: the surfaceless platform has pbuffer configs only,
        // and eglChooseConfig asks for window ones unless told otherwise.
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!p_eglChooseConfig(display, configAttribs, &config, 1, &count) || count < 1) {
            write_str("[graphics.compositor] gl: no GLES 3 config\n");
            return false;
        }
        const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext context = p_eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
        if (context == EGL_NO_CONTEXT || !p_eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
            write_str("[graphics.compositor] gl: no GLES 3 context\n");
            return false;
        }
        display_ = display;
        return true;
    }

    bool startProgram() {
        const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
        if (!vertex || !fragment) {
            return false;
        }
        program_ = p_glCreateProgram();
        p_glAttachShader(program_, vertex);
        p_glAttachShader(program_, fragment);
        p_glBindAttribLocation(program_, 0, "corner");
        p_glLinkProgram(program_);
        GLint linked = 0;
        p_glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) {
            write_str("[graphics.compositor] gl: the program does not link\n");
            return false;
        }
        p_glUseProgram(program_);
        u_.quad = p_glGetUniformLocation(program_, "u_quad");
        u_.uv = p_glGetUniformLocation(program_, "u_uv");
        u_.screen = p_glGetUniformLocation(program_, "u_screen");
        u_.mode = p_glGetUniformLocation(program_, "u_mode");
        u_.color = p_glGetUniformLocation(program_, "u_color");
        u_.tex = p_glGetUniformLocation(program_, "u_tex");
        u_.bgra = p_glGetUniformLocation(program_, "u_bgra");
        u_.outBgra = p_glGetUniformLocation(program_, "u_outBgra");
        u_.shape = p_glGetUniformLocation(program_, "u_shape");
        u_.radii = p_glGetUniformLocation(program_, "u_radii");
        u_.ring = p_glGetUniformLocation(program_, "u_ring");
        u_.shadow = p_glGetUniformLocation(program_, "u_shadow");
        p_glUniform2f(u_.screen, static_cast<float>(width_), static_cast<float>(height_));
        p_glUniform1i(u_.tex, 0);
        p_glUniform1i(u_.outBgra, readBack_ ? 1 : 0);

        // The unit quad, as two triangles.
        static const float corners[12] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
        GLuint buffer = 0;
        p_glGenBuffers(1, &buffer);
        p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        p_glEnableVertexAttribArray(0);
        p_glDisable(GL_DITHER);   // on by default; the targets are 8 bits a channel
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied
        p_glActiveTexture(GL_TEXTURE0);
        return true;
    }

    // The scanout buffers as renderbuffers, or (read back) one of our own.
    bool startTargets(const long* fds) {
        const std::uint32_t targets = readBack_ ? 1 : kBuffers;
        for (std::uint32_t i = 0; i < targets; ++i) {
            GLuint renderbuffer = 0;
            p_glGenRenderbuffers(1, &renderbuffer);
            p_glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            if (readBack_) {
                p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width_, height_);
            } else {
                const EGLint attribs[] = {
                    EGL_WIDTH, width_, EGL_HEIGHT, height_, EGL_LINUX_DRM_FOURCC_EXT, kFourccXrgb8888,
                    EGL_DMA_BUF_PLANE0_FD_EXT, static_cast<EGLint>(fds[i]), EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(pitch_), EGL_NONE};
                EGLImageKHR image = p_eglCreateImageKHR(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
                if (image == EGL_NO_IMAGE_KHR || !p_glEGLImageTargetRenderbufferStorageOES) {
                    write_str("[graphics.compositor] gl: a scanout buffer does not import\n");
                    return false;
                }
                p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
            }
            p_glGenFramebuffers(1, &framebuffers_[i]);
            p_glBindFramebuffer(GL_FRAMEBUFFER, framebuffers_[i]);
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
            if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                write_str("[graphics.compositor] gl: a target framebuffer is incomplete\n");
                return false;
            }
        }
        return true;
    }

    void note(const CursorState& cursor, const CursorImage& image) {
        cursor_ = cursor;
        cursorImage_ = &image;
        pending_ = true;
    }

    // Uniform helpers ---------------------------------------------------------
    static void colourOf(ui::Color c, float* out) {
        out[0] = c.r / 255.0f;
        out[1] = c.g / 255.0f;
        out[2] = c.b / 255.0f;
        out[3] = c.a / 255.0f;
    }

    void shape(const Rect& r, float tl, float tr, float br, float bl, float ring = 0.0f) {
        p_glUniform4f(u_.shape, static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width),
                      static_cast<float>(r.height));
        p_glUniform4f(u_.radii, tl, tr, br, bl);
        p_glUniform1f(u_.ring, ring);
    }

    void quad(const Rect& r) {
        p_glUniform4f(u_.quad, static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width),
                      static_cast<float>(r.height));
        p_glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    void fill(const Rect& r, ui::Color colour, float radius, float ring = 0.0f) {
        float c[4];
        colourOf(colour, c);
        p_glUniform1i(u_.mode, 0);
        p_glUniform4f(u_.color, c[0], c[1], c[2], c[3]);
        shape(r, radius, radius, radius, radius, ring);
        quad(r);
    }

    // `texture`'s texels (sx, sy, w, h) onto `r`, cut by the shape set before.
    void textured(const Texture& texture, const Rect& r, int sx, int sy, int mode, bool bgra) {
        p_glUniform1i(u_.mode, mode);
        p_glUniform1i(u_.bgra, bgra ? 1 : 0);
        p_glUniform4f(u_.color, 1, 1, 1, 1);
        const float tw = static_cast<float>(texture.width), th = static_cast<float>(texture.height);
        p_glUniform4f(u_.uv, sx / tw, sy / th, (sx + r.width) / tw, (sy + r.height) / th);
        p_glBindTexture(GL_TEXTURE_2D, texture.name);
        quad(r);
    }

    // Textures ------------------------------------------------------------------
    static void makeTexture(Texture* texture, int width, int height) {
        if (texture->name != 0 && texture->width == width && texture->height == height) {
            return;
        }
        if (texture->name == 0) {
            p_glGenTextures(1, &texture->name);
        }
        p_glBindTexture(GL_TEXTURE_2D, texture->name);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        texture->width = width;
        texture->height = height;
    }

    // BGRA pixels (`pitch` of them a row) into the texture at (0, 0).
    static void upload(Texture* texture, const std::uint32_t* pixels, int pitch, int width, int height) {
        makeTexture(texture, width, height);
        p_glBindTexture(GL_TEXTURE_2D, texture->name);
        p_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // The side buffer's `r` (already drawn) into `texture`.
    void uploadSide(Texture* texture, const Rect& r) {
        upload(texture, side_ + static_cast<std::size_t>(r.y) * width_ + r.x, width_, r.width, r.height);
    }

    ui::Canvas sideCanvas() { return ui::Canvas(side_, width_, height_, width_); }

    SurfaceTexture* surfaceTexture(const SurfaceCacheEntry& entry) {
        SurfaceTexture* free = nullptr;
        for (SurfaceTexture& s : surfaces_) {
            if (s.id == entry.id && s.texture.name != 0) {
                return &s;
            }
            if (!free && s.id == 0) {
                free = &s;
            }
        }
        if (!free) {
            return nullptr;
        }
        *free = {};
        free->id = entry.id;
        // A GPU-buffer surface is sampled where it is.
        const std::uint64_t handle = dmaBufImport_ ? std::surface_export_buffer(entry.id) : static_cast<std::uint64_t>(-1);
        if (!std::syscall_failed(handle)) {
            const int fd = instantos_handle_fd(static_cast<long>(handle));
            const EGLint attribs[] = {
                EGL_WIDTH, static_cast<EGLint>(entry.info.width), EGL_HEIGHT, static_cast<EGLint>(entry.info.height),
                EGL_LINUX_DRM_FOURCC_EXT, kFourccXrgb8888, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(entry.info.pitch),
                EGL_NONE};
            free->image = fd >= 0 ? p_eglCreateImageKHR(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs)
                                  : EGL_NO_IMAGE_KHR;
            if (fd >= 0) {
                close(fd);
            }
            if (free->image != EGL_NO_IMAGE_KHR) {
                ++stats_.imports;
                p_glGenTextures(1, &free->texture.name);
                p_glBindTexture(GL_TEXTURE_2D, free->texture.name);
                p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, free->image);
                free->texture.width = static_cast<int>(entry.info.width);
                free->texture.height = static_cast<int>(entry.info.height);
                return free;
            }
            if (importFailures_++ < 5) {
                write_str("[graphics.compositor] gl: a window's buffer did not import (EGL error ");
                write_u64(static_cast<std::uint64_t>(p_eglGetError()));
                write_str("); drawing it from its mapping\n");
            }
        }
        free->shared = true;
        makeTexture(&free->texture, static_cast<int>(entry.info.width), static_cast<int>(entry.info.height));
        return free;
    }

    // A surface the compositor still knows (a window's swapchain has several,
    // one on screen at a time) keeps its texture: making it again means
    // exporting and importing the buffer.
    bool cached(std::uint64_t id) const {
        for (std::uint64_t i = 0; i < kMaxSurfaceCache; ++i) {
            if (cache_[i].valid && cache_[i].id == id) {
                return true;
            }
        }
        return false;
    }

    void forgetUnseen() {
        for (SurfaceTexture& s : surfaces_) {
            if (s.seen) {
                s.lastSeen = frameNumber_;
            }
            // Kept a while after its last use: a swapchain's other buffers
            // leave the compositor's cache whenever another is shown, and come
            // back a frame later.
            if (s.id != 0 && !s.seen && !cached(s.id) && frameNumber_ - s.lastSeen > kTextureKeepFrames) {
                if (s.texture.name) p_glDeleteTextures(1, &s.texture.name);
                if (s.image != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(display_, s.image);
                s = {};
            }
            s.seen = false;
        }
        for (TitleTexture& t : titles_) {
            if (t.windowId != 0 && !t.seen) {
                if (t.texture.name) p_glDeleteTextures(1, &t.texture.name);
                t = {};
            }
            t.seen = false;
        }
    }

    TitleTexture* titleTexture(std::uint64_t windowId) {
        TitleTexture* free = nullptr;
        for (TitleTexture& t : titles_) {
            if (t.windowId == windowId) {
                return &t;
            }
            if (!free && t.windowId == 0) {
                free = &t;
            }
        }
        if (free) {
            *free = {};
            free->windowId = windowId;
        }
        return free;
    }

    // The frame ------------------------------------------------------------------
    void drawFrame() {
        ++frameNumber_;
        const ui::Theme& t = ui::theme();
        p_glClearColor(0, 0, 0, 1);
        p_glClear(GL_COLOR_BUFFER_BIT);
        const Rect screen = {0, 0, width_, height_};

        // The wallpaper, again only when it changed.
        if (background_ && background_->ready && background_->pixels) {
            if (wallpaperPixels_ != background_->pixels || std::strcmp(wallpaperPath_, background_->currentPath) != 0) {
                upload(&wallpaper_, background_->pixels, static_cast<int>(background_->width),
                       static_cast<int>(background_->width), static_cast<int>(background_->height));
                wallpaperPixels_ = background_->pixels;
                std::strncpy(wallpaperPath_, background_->currentPath, sizeof(wallpaperPath_) - 1);
            }
            shape(screen, 0, 0, 0, 0);
            textured(wallpaper_, screen, 0, 0, 1, true);
        }

        const std::uint64_t count = *windowCount_;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::WindowInfo& window = windows_[i];
            if (!is_window_visible(window)) {
                continue;
            }
            drawWindow(window, t);
        }
        drawTaskbar(t);
        drawCursor();
        forgetUnseen();
    }

    void drawWindow(const std::WindowInfo& window, const ui::Theme& t) {
        const bool focused = (window.state & std::WindowStateFocused) != 0;
        const Rect frame = {window.x, window.y, frame_width(window), frame_height(window)};
        const float radius = static_cast<float>(kWindowCornerRadius);
        const Rect inner = frame.inset(kBorder);
        const float innerRadius = static_cast<float>(kWindowCornerRadius - kBorder);

        // The shadow, outside the frame (the body covers the rest).
        const Rect shadowShape = frame.offset(0, kShadowOffsetY);
        p_glUniform1i(u_.mode, 3);
        p_glUniform2f(u_.shadow, (focused ? 120.0f : 70.0f) / 255.0f, static_cast<float>(kShadowBlur));
        shape(shadowShape, radius, radius, radius, radius);
        quad({shadowShape.x - kShadowBlur, shadowShape.y - kShadowBlur, shadowShape.width + 2 * kShadowBlur,
              shadowShape.height + 2 * kShadowBlur});

        // The body and its border.
        fill(frame, t.window, radius);
        fill(frame, ui::rgba(0xffffff, focused ? 26 : 15), radius, 1.0f);

        // The title bar, drawn by the CPU over the window colour.
        const Rect titleRect = Rect{inner.x, inner.y, inner.width, kTitleBarHeight - kBorder}.intersect(
            {0, 0, width_, height_});
        if (!titleRect.empty()) {
            if (TitleTexture* title = titleTexture(window.id)) {
                title->seen = true;
                // Drawn again only when what it shows changed.
                Fingerprint key;
                key.add(titleRect.width).add(titleRect.height).add(titleRect.x - inner.x).add(titleRect.y - inner.y);
                key.add(window.state & (std::WindowStateFocused | std::WindowStateMaximized));
                key.add(gWindowHover.windowId == window.id ? gWindowHover.control : -1);
                key.add(window.title, sizeof(window.title));
                if (title->key != key.value || title->texture.name == 0) {
                    ui::Canvas side = sideCanvas().clipped(titleRect);
                    side.fill(titleRect, t.window);
                    draw_window_title(side, window, controls_);
                    uploadSide(&title->texture, titleRect);
                    title->key = key.value;
                    ++stats_.titleUploads;
                }
                shape(inner, innerRadius, innerRadius, innerRadius, innerRadius);
                textured(title->texture, titleRect, 0, 0, 1, true);
            }
        }

        // The contents, cut to the rounded bottom corners.
        const SurfaceCacheEntry* entry = find_surface_cache(cache_, window.surfaceID);
        if (!entry || !entry->valid || entry->info.width == 0 || entry->info.height == 0) {
            return;
        }
        SurfaceTexture* surface = surfaceTexture(*entry);
        if (!surface) {
            return;
        }
        surface->seen = true;
        if (surface->shared && surface->uploaded != entry->generation && entry->info.address) {
            const int pitch = static_cast<int>(entry->info.pitch >= 4 ? entry->info.pitch / 4 : entry->info.width);
            upload(&surface->texture, reinterpret_cast<const std::uint32_t*>(entry->info.address), pitch,
                   static_cast<int>(entry->info.width), static_cast<int>(entry->info.height));
            surface->uploaded = entry->generation;
            ++stats_.windowUploads;
        }
        const int w = static_cast<int>(entry->info.width) < window.width ? static_cast<int>(entry->info.width) : window.width;
        const int h = static_cast<int>(entry->info.height) < window.height ? static_cast<int>(entry->info.height)
                                                                          : window.height;
        const Rect content = {window.x + kBorder, window.y + kTitleBarHeight, w, h};
        const Rect innerShape = {window.x + kBorder, window.y + kBorder, window.width, frame_height(window) - 2 * kBorder};
        shape(innerShape, 0, 0, innerRadius, innerRadius);
        // A shared-memory surface is BGRA bytes in an RGBA texture; an imported
        // XRGB8888 one samples as RGB already.
        textured(surface->texture, content, 0, 0, 1, surface->shared);
    }

    void drawTaskbar(const ui::Theme& t) {
        const std::uint64_t count = *windowCount_;
        const TaskbarLayout layout =
            taskbar_layout(static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_), windows_, count);
        const Rect bar = layout.bar.intersect({0, 0, width_, height_});
        if (!bar.empty()) {
            // Drawn again only when what it shows changed: the windows (tiles,
            // dots, the focused one), the hovered tile, the network icon, the
            // wallpaper under it, the clock's minute.
            const std::Time::DateTime now = std::Time::now();
            Fingerprint key;
            key.add(bar.x).add(bar.y).add(bar.width).add(bar.height).add(gTaskbarHover).add(gNetworkOnline ? 1 : 0);
            key.add(reinterpret_cast<std::intptr_t>(background_ ? background_->barBlur : nullptr));
            key.add(now.year).add(now.month).add(now.day).add(now.hour).add(now.minute);
            for (std::uint64_t i = 0; i < count; ++i) {
                key.add(static_cast<std::int64_t>(windows_[i].id)).add(windows_[i].state);
                key.add(windows_[i].title, sizeof(windows_[i].title));
            }
            if (key.value != taskbarKey_ || taskbarTexture_.name == 0) {
                ui::Canvas side = sideCanvas().clipped(bar);
                draw_taskbar(side, windows_, count, taskbar_, background_);
                uploadSide(&taskbarTexture_, bar);
                taskbarKey_ = key.value;
                ++stats_.taskbarUploads;
            }
            const float r = static_cast<float>(kTaskbarRadius);
            shape(layout.bar, r, r, r, r);
            textured(taskbarTexture_, bar, 0, 0, 1, true);
        }
        // The hovered tile's tooltip, over the windows.
        if (gTaskbarHover >= 0 && gTaskbarHover < layout.tileCount) {
            const Rect tip = taskbar_tooltip_rect(layout, gTaskbarHover, windows_, count).intersect({0, 0, width_, height_});
            if (!tip.empty()) {
                ui::Canvas side = sideCanvas().clipped(tip);
                side.fill(tip, ui::Color{t.tooltip.b, t.tooltip.g, t.tooltip.r, 255});
                draw_taskbar(side, windows_, count, taskbar_, background_);
                uploadSide(&tooltipTexture_, tip);
                shape(tip, 8, 8, 8, 8);
                textured(tooltipTexture_, tip, 0, 0, 1, true);
            }
        }
    }

    void drawCursor() {
        if (!cursorImage_ || !cursorImage_->ready()) {
            return;
        }
        const ui::Image& image = cursorImage_->image;
        if (!cursorUploaded_) {
            upload(&cursorTexture_, reinterpret_cast<const std::uint32_t*>(image.pixels()), image.width(), image.width(),
                   image.height());
            cursorUploaded_ = true;
        }
        const Rect r = {cursor_.x - cursorImage_->hotspotX, cursor_.y - cursorImage_->hotspotY, image.width(),
                        image.height()};
        shape({-100000, -100000, 200000, 200000}, 0, 0, 0, 0);
        textured(cursorTexture_, r, 0, 0, 2, true);
    }

    // The emulation: the frame (BGRA already) out into the mapped buffer.
    void readBack(std::uint32_t back) {
        p_glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(pitch_ / 4));
        p_glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, mapped_[back]);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    }

    FramebufferView target_ = {};
    int width_ = 0;
    int height_ = 0;
    std::uint32_t pitch_ = 0;
    std::uint32_t* mapped_[kBuffers] = {};
    GLuint framebuffers_[kBuffers] = {};
    bool readBack_ = false;
    bool dmaBufImport_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    GLuint program_ = 0;
    Uniforms u_ = {};
    std::uint32_t front_ = kBuffers;
    bool pending_ = false;
    std::uint32_t importFailures_ = 0;
    std::uint64_t frameNumber_ = 0;
    struct Stats {
        std::uint64_t frames, recordMs, finishMs, imports, windowUploads, titleUploads, taskbarUploads;
    } stats_ = {};
    std::uint32_t* side_ = nullptr;

    const std::WindowInfo* windows_ = nullptr;
    const std::uint64_t* windowCount_ = nullptr;
    SurfaceCacheEntry* cache_ = nullptr;
    const DesktopBackground* background_ = nullptr;
    const TaskbarAssets* taskbar_ = nullptr;
    const WindowControlAssets* controls_ = nullptr;
    CursorState cursor_ = {};
    const CursorImage* cursorImage_ = nullptr;

    Texture wallpaper_;
    const std::uint32_t* wallpaperPixels_ = nullptr;
    char wallpaperPath_[192] = {};
    Texture taskbarTexture_;
    std::uint64_t taskbarKey_ = 0;
    Texture tooltipTexture_;
    Texture cursorTexture_;
    bool cursorUploaded_ = false;
    SurfaceTexture surfaces_[kMaxTextures];
    TitleTexture titles_[kMaxTextures];
};

GlBackend g_gl;

}  // namespace

CompositorBackend* gl_backend_start(bool acceptEmulation) {
    return g_gl.start(acceptEmulation) ? &g_gl : nullptr;
}

void gl_backend_set_scene(const std::WindowInfo* windows, const std::uint64_t* count, SurfaceCacheEntry* cache,
                          const DesktopBackground* background, const TaskbarAssets* taskbar,
                          const WindowControlAssets* controls) {
    g_gl.setScene(windows, count, cache, background, taskbar, controls);
}

#endif  // INSTANTOS_COMPOSITOR_GL
