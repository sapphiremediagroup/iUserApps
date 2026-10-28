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

bool launch_browser() {
    return std::spawn("/bin/netsurf") != fail;
}

bool launch_taskbar_slot(int slot) {
    switch (slot) {
        case 0:
            return launch_terminal();
        case 1:
            return launch_file_browser();
        case 2:
            return launch_background_switcher();
        case 3:
            return launch_cube();
        case 4:
            return launch_browser();
        default:
            return false;
    }
}

const char* taskbar_slot_label(int slot) {
    static const char* const labels[kTaskbarCenterSlotCount] = { "Terminal", "Files", "Walls", "Cube", "Web" };
    return slot >= 0 && slot < kTaskbarCenterSlotCount ? labels[slot] : "";
}

const char* taskbar_slot_window_title(int slot) {
    static const char* const titles[kTaskbarCenterSlotCount] = {
        "Terminal", "File Browser", "Background Switcher", "Cube", "NetSurf" };
    return slot >= 0 && slot < kTaskbarCenterSlotCount ? titles[slot] : "";
}

volatile bool gNetworkOnline = false;

namespace {
std::Handle gNetworkWakeQueue = fail;

// Blocks in NetGetConfig until the configuration changes; on a change of
// online/offline it pokes the compositor's queue so the taskbar is redrawn.
void watch_network(void*) {
    std::uint64_t generation = 0;
    for (;;) {
        std::NetConfig config = {};
        const std::uint64_t result = _syscall_impl(static_cast<std::uint64_t>(std::Syscall::NetGetConfig),
            reinterpret_cast<std::uint64_t>(&config), generation, static_cast<std::uint64_t>(-1));
        if (result >= static_cast<std::uint64_t>(-4095)) {
            if (result == static_cast<std::uint64_t>(-2)) {
                return;   // no network device
            }
            std::sleep(1000);
            continue;
        }
        generation = config.generation;
        const bool online = (config.flags & std::NetConfigUp) != 0;
        if (online != gNetworkOnline) {
            gNetworkOnline = online;
            std::IPCMessage wake = {};   // neither event nor request: just wakes the loop
            std::queue_send(gNetworkWakeQueue, &wake, false);
        }
    }
}
}

void start_network_watch(std::Handle wakeQueue) {
    gNetworkWakeQueue = wakeQueue;
    if (std::thread_create(watch_network) == fail) {
        write_str("[graphics.compositor] network watch thread failed\n");
    }
}

bool decode_event_message(const std::IPCMessage& message, std::Event* event) {
    return std::event_from_message(message, event);
}
