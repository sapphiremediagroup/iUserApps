#pragma once

#include <cstdio.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

namespace service_daemon {

inline constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);

inline void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
}

inline void write_service_line(const char* serviceName, const char* suffix) {
    write_str("[");
    write_str(serviceName);
    write_str("] ");
    write_str(suffix);
}

inline bool build_hello_reply(
    const char* serviceName,
    const std::IPCMessage& message,
    char* replyBuffer,
    std::uint64_t* replySize
) {
    if (replyBuffer == nullptr || replySize == nullptr) {
        return false;
    }

    std::services::graphics_compositor::HelloRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::services::graphics_compositor::HelloReply reply = {};
    reply.header.version = std::services::graphics_compositor::VERSION;
    reply.header.opcode = static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::Hello);
    reply.status = std::services::STATUS_OK;
    std::strncpy(reply.service_name, serviceName, sizeof(reply.service_name) - 1);
    reply.service_name[sizeof(reply.service_name) - 1] = '\0';

    if (request.header.version != std::services::graphics_compositor::VERSION) {
        reply.status = std::services::STATUS_BAD_VERSION;
    } else if (request.header.opcode != static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::Hello)) {
        reply.status = std::services::STATUS_BAD_OPCODE;
    }

    std::memcpy(replyBuffer, &reply, sizeof(reply));
    *replySize = sizeof(reply);
    return true;
}

inline int run(const char* serviceName) {
    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_service_line(serviceName, "queue_create failed\n");
        return 1;
    }

    if (std::service_register(serviceName, queue) == fail) {
        write_service_line(serviceName, "service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_service_line(serviceName, "ready\n");

    for (;;) {
        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, true) == fail) {
            write_service_line(serviceName, "queue_receive failed\n");
            break;
        }

        if ((message.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            char reply[sizeof(std::services::graphics_compositor::HelloReply)] = {};
            std::uint64_t replySize = 0;
            if (!build_hello_reply(serviceName, message, reply, &replySize)) {
                write_service_line(serviceName, "invalid request payload\n");
                break;
            }

            if (std::queue_reply(queue, message.id, reply, replySize) == fail) {
                write_service_line(serviceName, "queue_reply failed\n");
                break;
            }
            continue;
        }
    }

    std::close(queue);
    return 1;
}

}
