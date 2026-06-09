#include <stdio.h>
#include <syscall.hpp>

namespace {

bool syscall_failed(std::uint64_t result)
{
    return result >= static_cast<std::uint64_t>(-4095);
}

int syscall_errno(std::uint64_t result)
{
    return static_cast<int>(-static_cast<std::int64_t>(result));
}

std::uint64_t syscall(std::Syscall number, std::uint64_t arg1 = 0, std::uint64_t arg2 = 0, std::uint64_t arg3 = 0)
{
    return _syscall_impl(static_cast<std::uint64_t>(number), arg1, arg2, arg3);
}

}

int main(int, char**)
{
    printf("net-smoke: start\n");

    const std::uint64_t link = syscall(std::Syscall::NetLinkStatus);
    printf("net-smoke: link=%llu\n", static_cast<unsigned long long>(link));

    unsigned char mac[6] {};
    const std::uint64_t mac_result = syscall(std::Syscall::NetGetMAC, reinterpret_cast<std::uint64_t>(mac));
    if (syscall_failed(mac_result)) {
        printf("net-smoke: no virtio-net errno=%d\n", syscall_errno(mac_result));
        return 0;
    }
    printf("net-smoke: mac=%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    unsigned char frame[1514] {};
    const std::uint64_t recv_result = syscall(
        std::Syscall::NetRecv,
        reinterpret_cast<std::uint64_t>(frame),
        sizeof(frame)
    );
    if (!syscall_failed(recv_result)) {
        printf("net-smoke: recv=%llu\n", static_cast<unsigned long long>(recv_result));
    } else {
        printf("net-smoke: recv-empty errno=%d\n", syscall_errno(recv_result));
    }

    constexpr std::uint64_t gateway = (10ULL << 24) | (0ULL << 16) | (2ULL << 8) | 2ULL;
    const std::uint64_t ping_result = syscall(std::Syscall::NetPing, gateway, 1, 1);
    printf("net-smoke: ping-result=%lld\n", static_cast<long long>(ping_result));
    for (int i = 0; i < 32; ++i) {
        syscall(std::Syscall::NetProcessPackets);
        _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), 10);
    }

    struct PingReply {
        std::uint32_t srcIp;
        std::uint16_t id;
        std::uint16_t seq;
        std::uint16_t payloadSize;
        std::uint16_t reserved;
    } reply {};
    const std::uint64_t reply_result = syscall(
        std::Syscall::NetGetPingReply,
        reinterpret_cast<std::uint64_t>(&reply)
    );
    if (syscall_failed(reply_result)) {
        printf("net-smoke: ping-reply errno=%d\n", syscall_errno(reply_result));
    } else {
        printf("net-smoke: ping-reply src=%u id=%u seq=%u payload=%u\n",
            reply.srcIp, reply.id, reply.seq, reply.payloadSize);
    }

    printf("net-smoke: done\n");
    return 0;
}
