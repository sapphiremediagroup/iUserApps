#include <cstdio.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

}

int main() {
    write_str("[input.manager] phase=enter\n");
    const std::Handle queue = std::event_queue_create();
    if (queue == fail) {
        write_str("[input.manager] event_queue_create failed\n");
        return 1;
    }
    write_str("[input.manager] phase=queue-created\n");

    write_str("[input.manager] phase=registering\n");
    if (std::service_register(std::services::input_manager::NAME, queue) == fail) {
        write_str("[input.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_str("[input.manager] phase=registered\n");
    write_str("[input.manager] ready\n");

    for (;;) {
        std::IPCMessage ignored = {};
        const std::uint64_t wait_result = std::queue_receive(queue, &ignored, true);
        if (wait_result == fail) {
            std::yield();
        }
    }

    std::close(queue);
    return 0;
}
