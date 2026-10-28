#include "common.hpp"
#include "backend.hpp"

class GraphicsCompositor {
public:
    GraphicsCompositor() = default;
    GraphicsCompositor(const GraphicsCompositor&) = delete;
    GraphicsCompositor& operator=(const GraphicsCompositor&) = delete;

    ~GraphicsCompositor() {
        delete[] background.pixels;
        if (queue != fail) {
            std::close(queue);
        }
    }

    int run(int argc, char** argv) {
        if (!initializeService() ||
            !initializeFramebuffer() ||
            !initializeBackend(argc, argv) ||
            !initializeFonts() ||
            !initializeBuffers()) {
            return 1;
        }

        initializePointerState();
        initializeAssets();
        renderInitialScene();
        start_network_watch(queue);
        write_str("[graphics.compositor] ready\n");
        eventLoop();
        return 1;
    }

private:
    std::Handle queue = fail;
    std::FBInfo fbInfo = {};
    FramebufferView framebuffer = {};
    CompositorBackend* backend = nullptr;
    TripleBufferState buffers = {};
    ui::Canvas* sceneBuffer = nullptr;
    CursorState cursor = {};
    DragState drag = {};
    std::uint16_t pointerButtons = 0;
    DesktopBackground background = {};
    CursorImage cursorImage = {};
    ui::Image taskbarBrand;
    TaskbarStatusAssets taskbarStatus = {};
    WindowControlAssets windowControls = {};
    std::WindowInfo windowSnapshot[kMaxWindows] = {};
    std::uint64_t windowSnapshotCount = 0;
    bool windowSnapshotReady = false;
    bool shownNetworkOnline = false;
    PointerRoute pointerRoute = {};

    bool initializeService() {
        queue = std::queue_create();
        if (queue == fail) {
            write_str("[graphics.compositor] queue_create failed\n");
            return false;
        }

        if (std::service_register(std::services::graphics_compositor::NAME, queue) == fail) {
            write_str("[graphics.compositor] service_register failed\n");
            return false;
        }
        return true;
    }

    bool initializeFramebuffer() {
        if (std::fb_info(&fbInfo) == fail) {
            write_str("[graphics.compositor] fb_info failed\n");
            return false;
        }

        auto* pixels = static_cast<std::uint32_t*>(std::fb_map());
        if (pixels == reinterpret_cast<std::uint32_t*>(fail)) {
            pixels = reinterpret_cast<std::uint32_t*>(fbInfo.addr);
        }
        if (pixels == nullptr || pixels == reinterpret_cast<std::uint32_t*>(fail)) {
            write_str("[graphics.compositor] fb_map failed\n");
            return false;
        }

        framebuffer = {
            pixels,
            fbInfo.width,
            fbInfo.height,
            fbInfo.pitch
        };
        return true;
    }

    bool initializeBackend(int argc, char** argv) {
        backend = compositor_backend_select(framebuffer, argc, argv);
        return backend != nullptr;
    }

    bool initializeFonts() {
        if (!ui::Font::ui().valid()) {
            write_str("[graphics.compositor] ui font load failed, continuing without text\n");
        }
        if (!gClockTimeFont.load(kClockTimeFontPath, kClockTimeFontPixelHeight)) {
            write_str("[graphics.compositor] clock time font load failed, using ui font\n");
        }
        if (!gClockDateFont.load(kClockDateFontPath, kClockDateFontPixelHeight)) {
            write_str("[graphics.compositor] clock date font load failed, using ui font\n");
        }
        return true;
    }

    bool initializeBuffers() {
        if (!buffers.initialize(fbInfo.width, fbInfo.height)) {
            write_str("[graphics.compositor] triple buffer allocation failed\n");
            return false;
        }
        return true;
    }

    void initializePointerState() {
        cursor.x = static_cast<int>(fbInfo.width / 2);
        cursor.y = static_cast<int>(fbInfo.height / 2);
        std::memset(gSurfaceCache, 0, sizeof(gSurfaceCache));
    }

    void initializeAssets() {
        load_first_available_background(&background, fbInfo.width, fbInfo.height);
        if (!cursorImage.loadFromFile(kDefaultCursorPath)) {
            write_str("[graphics.compositor] cursor load failed\n");
        }
        if (!taskbarBrand.load(kTaskbarBrandPath)) {
            write_str("[graphics.compositor] taskbar brand load failed\n");
        } else if (!taskbarBrand.resize(kTaskbarBrandWidth, kTaskbarBrandHeight)) {
            write_str("[graphics.compositor] taskbar brand resize failed\n");
        }
        taskbarStatus.load();
        windowControls.load();
    }

    void renderInitialScene() {
        sceneBuffer = &buffers.acquire();
        updateWindowSnapshot();
        redraw_scene(*sceneBuffer, windowSnapshot, windowSnapshotCount, gSurfaceCache, &background, &taskbarBrand, &taskbarStatus, &windowControls);
        backend->presentFull(*sceneBuffer, cursor, cursorImage);
        ++gTiming.frames;
    }

    void eventLoop() {
        for (;;) {
            bool fullSceneDirty = false;
            bool surfaceDirty = pumpSurfaceUpdatesOnce();
            Rect windowDirty = { 0, 0, 0, 0 };

            if (!surfaceDirty) {
                std::IPCMessage message = {};
                if (std::queue_receive(queue, &message, true) != fail) {
                    handleMessage(message, &fullSceneDirty, &windowDirty);
                }
            }

            drainMessages(&fullSceneDirty, &windowDirty);
            if (gNetworkOnline != shownNetworkOnline) {
                shownNetworkOnline = gNetworkOnline;
                fullSceneDirty = true;
            }

            if (pumpSurfaceUpdatesOnce()) {
                surfaceDirty = true;
            }
            if (updateWindowSnapshot()) {
                fullSceneDirty = true;
            }

            if (fullSceneDirty) {
                redrawFullScene();
                continue;
            }

            if (surfaceDirty) {
                const Rect dirty = consume_surface_dirty_rect(windowSnapshot, windowSnapshotCount, gSurfaceCache, fbInfo.width, fbInfo.height);
                windowDirty = windowDirty.unite(dirty);
            }

            if (!windowDirty.empty()) {
                redrawDirtyRect(windowDirty);
            }
        }
    }

    static bool sameWindow(const std::WindowInfo& a, const std::WindowInfo& b) {
        return a.id == b.id &&
               a.ownerPID == b.ownerPID &&
               a.flags == b.flags &&
               a.state == b.state &&
               a.x == b.x &&
               a.y == b.y &&
               a.width == b.width &&
               a.height == b.height &&
               a.surfaceID == b.surfaceID &&
               a.zOrder == b.zOrder &&
               std::strncmp(a.title, b.title, sizeof(a.title)) == 0;
    }

    bool updateWindowSnapshot() {
        std::WindowInfo current[kMaxWindows] = {};
        const std::uint64_t count = fetch_windows(current, kMaxWindows);
        bool changed = !windowSnapshotReady || count != windowSnapshotCount;

        if (!changed) {
            for (std::uint64_t i = 0; i < count; ++i) {
                if (!sameWindow(current[i], windowSnapshot[i])) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            logNewWindows(current, count);
            forget_unshown_surfaces(gSurfaceCache, current, count);
        }
        std::memset(windowSnapshot, 0, sizeof(windowSnapshot));
        for (std::uint64_t i = 0; i < count && i < kMaxWindows; ++i) {
            windowSnapshot[i] = current[i];
        }
        windowSnapshotCount = count;
        windowSnapshotReady = true;
        return changed;
    }

    // One serial line per window that was not there before: where it opened
    // (the content area starts kBorder right of x and kTitleBarHeight below y).
    void logNewWindows(const std::WindowInfo* current, std::uint64_t count) {
        for (std::uint64_t i = 0; i < count && i < kMaxWindows; ++i) {
            bool known = false;
            for (std::uint64_t j = 0; j < windowSnapshotCount && !known; ++j) {
                known = windowSnapshot[j].id == current[i].id;
            }
            if (known || current[i].x < 0 || current[i].y < 0) {
                continue;
            }
            char title[sizeof(current[i].title) + 1] = {};
            std::memcpy(title, current[i].title, sizeof(current[i].title));
            write_str("[graphics.compositor] window id=");
            write_u64(current[i].id);
            write_str(" x=");
            write_u64(static_cast<std::uint64_t>(current[i].x));
            write_str(" y=");
            write_u64(static_cast<std::uint64_t>(current[i].y));
            write_str(" w=");
            write_u64(static_cast<std::uint64_t>(current[i].width));
            write_str(" h=");
            write_u64(static_cast<std::uint64_t>(current[i].height));
            write_str(" title=");
            write_str(title);
            write_str("\n");
        }
    }

    bool pumpSurfaceUpdatesOnce() {
        return pump_surface_updates(gSurfaceCache);
    }

    void drainMessages(bool* fullSceneDirty, Rect* windowDirty) {
        for (;;) {
            std::IPCMessage message = {};
            if (std::queue_receive(queue, &message, false) == fail) {
                break;
            }
            handleMessage(message, fullSceneDirty, windowDirty);
        }
    }

    void handleMessage(const std::IPCMessage& message, bool* fullSceneDirty, Rect* windowDirty) {
        if ((message.flags & std::IPC_MESSAGE_EVENT) != 0) {
            handleEventMessage(message, fullSceneDirty, windowDirty);
            return;
        }

        if ((message.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            handleRequestMessage(message, fullSceneDirty);
        }
    }

    void handleEventMessage(const std::IPCMessage& message, bool* fullSceneDirty, Rect* windowDirty) {
        std::Event event = {};
        if (!decode_event_message(message, &event)) {
            return;
        }

        if (event.type == std::EventType::Pointer) {
            handlePointerEventMessage(event, fullSceneDirty, windowDirty);
            return;
        }

        if (event.type == std::EventType::Key && event.key.action == std::KeyEventAction::Press) {
            handleKeyPress(event);
        }
    }

    void handlePointerEventMessage(const std::Event& event, bool* fullSceneDirty, Rect* windowDirty) {
        // Before our own handling, which may start a drag from this press.
        route_pointer_to_window(event, windowSnapshot, windowSnapshotCount, fbInfo.width, fbInfo.height, drag, &pointerRoute);
        const CursorState previousCursor = cursor;
        move_cursor(framebuffer, &cursor, event.pointer.x, event.pointer.y);
        if (handle_pointer_event(event, windowSnapshot, windowSnapshotCount, fbInfo.width, fbInfo.height, &drag, &pointerButtons, windowDirty)) {
            *fullSceneDirty = true;
        } else if (!*fullSceneDirty && (!windowDirty || windowDirty->empty()) &&
                   (previousCursor.x != cursor.x || previousCursor.y != cursor.y)) {
            backend->presentCursorMove(*sceneBuffer, previousCursor, cursor, cursorImage);
        }
    }

    void handleKeyPress(const std::Event& event) {
        const bool superPressed = (event.key.modifiers & std::KeyModifierSuper) != 0;
        if (!superPressed) {
            return;
        }

        const std::uint16_t key = event.key.keycode;
        if (key == std::KeyT) {
            if (!launch_terminal()) {
                write_str("[graphics.compositor] FAIL spawn terminal\n");
            }
        } else if (key == std::KeyF) {
            if (!launch_file_browser()) {
                write_str("[graphics.compositor] FAIL spawn file-browser\n");
            }
        } else if (key == std::KeyB) {
            if (!launch_background_switcher()) {
                write_str("[graphics.compositor] FAIL spawn background-switcher\n");
            }
        } else if (key == std::KeyC) {
            if (!launch_cube()) {
                write_str("[graphics.compositor] FAIL spawn cube\n");
            }
        } else if (key == std::KeyW) {
            if (!launch_browser()) {
                write_str("[graphics.compositor] FAIL spawn netsurf\n");
            }
        }
    }

    void handleRequestMessage(const std::IPCMessage& message, bool* fullSceneDirty) {
        std::services::MessageHeader header = {};
        if (!std::services::decode_message(message, &header)) {
            write_str("[graphics.compositor] invalid request payload\n");
            return;
        }

        if (header.opcode == static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::Hello)) {
            handleHelloRequest(message);
        } else if (header.opcode == static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::SetBackground)) {
            handleSetBackgroundRequest(message, fullSceneDirty);
        }
    }

    void handleHelloRequest(const std::IPCMessage& message) {
        std::services::graphics_compositor::HelloReply reply = {};
        if (!build_hello_reply(message, &reply)) {
            write_str("[graphics.compositor] invalid hello payload\n");
            return;
        }

        if (std::queue_reply(queue, message.id, &reply, sizeof(reply)) == fail) {
            write_str("[graphics.compositor] queue_reply failed\n");
        }
    }

    void handleSetBackgroundRequest(const std::IPCMessage& message, bool* fullSceneDirty) {
        std::services::graphics_compositor::SetBackgroundReply reply = {};
        if (!build_background_reply(message, &reply, &background, fbInfo.width, fbInfo.height, fullSceneDirty)) {
            write_str("[graphics.compositor] invalid background payload\n");
            return;
        }

        if (std::queue_reply(queue, message.id, &reply, sizeof(reply)) == fail) {
            write_str("[graphics.compositor] queue_reply failed\n");
        }
    }

    void redrawFullScene() {
        sceneBuffer = &buffers.acquire();
        updateWindowSnapshot();
        redraw_scene(*sceneBuffer, windowSnapshot, windowSnapshotCount, gSurfaceCache, &background, &taskbarBrand, &taskbarStatus, &windowControls);
        clear_surface_dirty_flags(gSurfaceCache);
        backend->presentFull(*sceneBuffer, cursor, cursorImage);
        ++gTiming.frames;
        report_timing_if_needed();
    }

    void redrawDirtyRect(Rect windowDirty) {
        windowDirty = clamp_rect(windowDirty, fbInfo.width, fbInfo.height);
        if (windowDirty.empty()) {
            return;
        }

        redraw_scene_rect(*sceneBuffer, windowSnapshot, windowSnapshotCount, gSurfaceCache, &background, windowDirty, &taskbarBrand, &taskbarStatus, &windowControls);
        backend->presentRect(*sceneBuffer, windowDirty, cursor, cursorImage);
        ++gTiming.frames;
        report_timing_if_needed();
    }
};

int main(int argc, char** argv) {
    GraphicsCompositor compositor;
    return compositor.run(argc, argv);
}
