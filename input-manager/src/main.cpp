#include <cstdio.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

void write_dec(std::uint64_t value) {
    char buf[21];
    int pos = 0;
    if (value == 0) {
        write_str("0");
        return;
    }

    while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        char c[2] = { buf[--pos], '\0' };
        write_str(c);
    }
}

void write_hex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    write_str("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        char c[2] = { digits[(value >> shift) & 0xF], '\0' };
        write_str(c);
    }
}
}

int main() {
    const std::Handle queue = std::event_queue_create();
    if (queue == fail) {
        write_str("[input.manager] event_queue_create failed\n");
        return 1;
    }
    write_str("[input.manager] service queue handle=");
    write_hex(queue);
    write_str("\n");

    if (std::service_register(std::services::input_manager::NAME, queue) == fail) {
        write_str("[input.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_str("[input.manager] ready\n");

    const std::Handle idle_queue = std::event_queue_create();
    if (idle_queue == fail) {
        write_str("[input.manager] idle queue create failed; yielding forever\n");
    } else {
        write_str("[input.manager] idle queue handle=");
        write_hex(idle_queue);
        write_str("\n");
    }
    for (;;) {
        if (idle_queue == fail) {
            std::yield();
            continue;
        }

        std::IPCMessage ignored = {};
        const std::uint64_t wait_result = std::event_wait(idle_queue, &ignored);
        if (wait_result == fail) {
            write_str("[input.manager] idle event_wait failed\n");
        } else {
            write_str("[input.manager] unexpected idle event size=");
            write_dec(ignored.size);
            write_str("\n");
        }
    }

    if (idle_queue != fail) {
        std::close(idle_queue);
    }
    std::close(queue);
    return 0;
}
