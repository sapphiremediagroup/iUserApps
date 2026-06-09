#include <stdio.h>

#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {

constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t snapshotCapacity = 32;

bool syscall_failed(std::uint64_t result)
{
    return result >= static_cast<std::uint64_t>(-4095);
}

int syscall_errno(std::uint64_t result)
{
    return static_cast<int>(-static_cast<std::int64_t>(result));
}

const char* state_name(std::uint32_t state)
{
    switch (state) {
        case 0: return "ready";
        case 1: return "running";
        case 2: return "blocked";
        case 3: return "terminated";
    }
    return "unknown";
}

void write_str(const char* s)
{
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

bool handle_request(std::Handle queue, const std::IPCMessage& message)
{
    std::services::graphics_compositor::HelloReply reply = {};
    reply.header.version = std::services::process_manager::VERSION;
    reply.header.opcode = 1;
    reply.status = std::services::STATUS_OK;
    std::strncpy(reply.service_name, std::services::process_manager::NAME, sizeof(reply.service_name) - 1);

    return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
}

void print_snapshot()
{
    std::ProcInfoEntry entries[snapshotCapacity] {};
    std::uint64_t total = 0;
    const std::uint64_t copied = _syscall_impl(
        static_cast<std::uint64_t>(std::Syscall::ProcInfo),
        reinterpret_cast<std::uint64_t>(entries),
        snapshotCapacity,
        reinterpret_cast<std::uint64_t>(&total)
    );

    if (syscall_failed(copied)) {
        printf("[process.manager] ProcInfo errno=%d\n", syscall_errno(copied));
        return;
    }

    printf("[process.manager] processes total=%llu copied=%llu\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(copied));
    for (std::uint64_t i = 0; i < copied; ++i) {
        const std::ProcInfoEntry& entry = entries[i];
        printf("[process.manager] pid=%u ppid=%u uid=%u gid=%u sid=%u state=%s pri=%u flags=0x%x name=%s\n",
            entry.pid,
            entry.parentPID,
            entry.uid,
            entry.gid,
            entry.sessionID,
            state_name(entry.state),
            entry.priority,
            entry.flags,
            entry.name[0] ? entry.name : "<unnamed>");
    }
}

}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_str("[process.manager] queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::process_manager::NAME, queue) == fail) {
        write_str("[process.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_str("[process.manager] ready\n");
    print_snapshot();

    for (std::uint64_t iteration = 0;; ++iteration) {
        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, false) != fail && (message.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            if (!handle_request(queue, message)) {
                write_str("[process.manager] queue_reply failed\n");
            }
        }

        if ((iteration % 500) == 0) {
            print_snapshot();
        }

        _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 10);
    }

    std::close(queue);
    return 0;
}
