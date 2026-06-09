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

void write_str(const char* s)
{
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

bool has_flag(std::uint32_t flags, std::uint32_t flag)
{
    return (flags & flag) != 0;
}

bool handle_request(std::Handle queue, const std::IPCMessage& message)
{
    std::services::graphics_compositor::HelloReply reply = {};
    reply.header.version = std::services::storage_manager::VERSION;
    reply.header.opcode = 1;
    reply.status = std::services::STATUS_OK;
    std::strncpy(reply.service_name, std::services::storage_manager::NAME, sizeof(reply.service_name) - 1);

    return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
}

void print_storage_info()
{
    std::StorageInfo info = {};
    const std::uint64_t result = _syscall_impl(
        static_cast<std::uint64_t>(std::Syscall::StorageInfo),
        reinterpret_cast<std::uint64_t>(&info)
    );
    if (syscall_failed(result)) {
        printf("[storage.manager] StorageInfo errno=%d\n", syscall_errno(result));
        return;
    }

    if (!has_flag(info.flags, std::StorageInfoPresent)) {
        printf("[storage.manager] no persistent block device mount_error=%d\n", info.mountError);
        return;
    }

    printf("[storage.manager] device=%s size=%llu sector=%u readable=%u writable=%u mounted=%u formatted=%u fs=%s path=%s mount_error=%d\n",
        info.deviceName[0] ? info.deviceName : "<unknown>",
        static_cast<unsigned long long>(info.totalSize),
        info.sectorSize,
        has_flag(info.flags, std::StorageInfoReadable) ? 1u : 0u,
        has_flag(info.flags, std::StorageInfoWritable) ? 1u : 0u,
        has_flag(info.flags, std::StorageInfoMounted) ? 1u : 0u,
        has_flag(info.flags, std::StorageInfoFormatted) ? 1u : 0u,
        info.fsType[0] ? info.fsType : "<none>",
        info.mountPath[0] ? info.mountPath : "<none>",
        info.mountError);
}

int run_command(const char* command)
{
    std::Syscall syscall = std::Syscall::StorageInfo;
    if (std::strcmp(command, "format") == 0) {
        syscall = std::Syscall::StorageFormat;
    } else if (std::strcmp(command, "mount") == 0) {
        syscall = std::Syscall::StorageMount;
    } else {
        printf("[storage.manager] unknown command=%s\n", command ? command : "<null>");
        return 1;
    }

    const std::uint64_t result = _syscall_impl(static_cast<std::uint64_t>(syscall));
    if (syscall_failed(result)) {
        printf("[storage.manager] %s errno=%d\n", command, syscall_errno(result));
        print_storage_info();
        return 1;
    }

    printf("[storage.manager] %s ok\n", command);
    print_storage_info();
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc > 1) {
        return run_command(argv[1]);
    }

    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_str("[storage.manager] queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::storage_manager::NAME, queue) == fail) {
        write_str("[storage.manager] service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_str("[storage.manager] ready\n");
    print_storage_info();

    for (std::uint64_t iteration = 0;; ++iteration) {
        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, false) != fail && (message.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            if (!handle_request(queue, message)) {
                write_str("[storage.manager] queue_reply failed\n");
            }
        }

        if ((iteration % 1000) == 0) {
            print_storage_info();
        }

        _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 10);
    }

    std::close(queue);
    return 0;
}
