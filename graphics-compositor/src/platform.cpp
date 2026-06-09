#include "common.hpp"

void write_str(const char* s) {
    std::serial_write(s, std::strlen(s));
}

void write_u64(std::uint64_t value) {
    char digits[21] = {};
    int count = 0;
    if (value == 0) {
        std::serial_write("0", 1);
        return;
    }
    while (value > 0 && count < static_cast<int>(sizeof(digits))) {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0) {
        const char ch = digits[--count];
        std::serial_write(&ch, 1);
    }
}

void add_elapsed(std::uint64_t startMs, std::uint64_t* counter) {
    if (!counter) {
        return;
    }
    const std::uint64_t now = std::gettime();
    if (now >= startMs) {
        *counter += now - startMs;
    }
}

void report_timing_if_needed() {
    if (gTiming.frames == 0 || (gTiming.frames % 120ULL) != 0) {
        return;
    }

    write_str("[graphics.compositor] timing frames=");
    write_u64(gTiming.frames);
    write_str(" bg=");
    write_u64(gTiming.backgroundMs);
    write_str("ms frame=");
    write_u64(gTiming.windowFrameMs);
    write_str("ms blit=");
    write_u64(gTiming.surfaceBlitMs);
    write_str("ms redraw=");
    write_u64(gTiming.sceneRedrawMs);
    write_str("ms present=");
    write_u64(gTiming.presentMs);
    write_str("ms\n");
}

bool launch_file_browser() {
    return std::spawn("/bin/file-browser") != fail;
}

bool launch_cube() {
    return std::spawn("/bin/cube") != fail;
}

bool launch_background_switcher() {
    return std::spawn("/bin/background-switcher") != fail;
}

bool launch_terminal() {
    return std::spawn("/bin/terminal") != fail;
}

bool decode_event_message(const std::IPCMessage& message, std::Event* event) {
    return std::event_from_message(message, event);
}
