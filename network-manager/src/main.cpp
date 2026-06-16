#include <stdio.h>

#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace {

constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

bool syscall_failed(std::uint64_t result)
{
    return result >= static_cast<std::uint64_t>(-4095);
}

int syscall_errno(std::uint64_t result)
{
    return static_cast<int>(-static_cast<std::int64_t>(result));
}

std::uint64_t net_syscall(std::Syscall number, std::uint64_t arg1 = 0, std::uint64_t arg2 = 0)
{
    return _syscall_impl(static_cast<std::uint64_t>(number), arg1, arg2);
}

void write_str(const char* s)
{
    std::serial_write(s, std::strlen(s));
}

bool handle_request(std::Handle queue, const std::IPCMessage& message)
{
    std::services::graphics_compositor::HelloReply reply = {};
    reply.header.version = std::services::network_manager::VERSION;
    reply.header.opcode = 1;
    reply.status = std::services::STATUS_OK;
    std::strncpy(reply.service_name, std::services::network_manager::NAME, sizeof(reply.service_name) - 1);

    return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
}

void print_device_status()
{
    unsigned char mac[6] {};
    const std::uint64_t mac_result = net_syscall(std::Syscall::NetGetMAC, reinterpret_cast<std::uint64_t>(mac));
    if (syscall_failed(mac_result)) {
        printf("[network.manager] no virtio-net errno=%d\n", syscall_errno(mac_result));
        return;
    }

    const std::uint64_t link = net_syscall(std::Syscall::NetLinkStatus);
    printf("[network.manager] ready mac=%02x:%02x:%02x:%02x:%02x:%02x link=%llu\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], static_cast<unsigned long long>(link));
}

}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_str("[network.manager] queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::network_manager::NAME, queue) == fail) {
        write_str("[network.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    print_device_status();

    for (;;) {
        const std::uint64_t process_result = net_syscall(std::Syscall::NetProcessPackets);
        if (syscall_failed(process_result) && syscall_errno(process_result) != 11) {
            printf("[network.manager] NetProcessPackets errno=%d\n", syscall_errno(process_result));
        }

        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, false) != fail && (message.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            if (!handle_request(queue, message)) {
                write_str("[network.manager] queue_reply failed\n");
            }
        }

        _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 10);
    }

    std::close(queue);
    return 0;
}
