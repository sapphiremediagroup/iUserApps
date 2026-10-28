// What the network tools need from InstantOS beyond POSIX: the interface
// configuration syscalls (NetGetConfig/NetSetConfig) and their NetConfig
// struct, mirrored from include/cpu/syscall/syscall.hpp, and KernelLog (dmesg).
#pragma once

#include <stdint.h>

enum {
    INSTANTOS_SYS_NET_GET_MAC = 67,
    INSTANTOS_SYS_NET_LINK_STATUS = 70,
    INSTANTOS_SYS_NET_GET_CONFIG = 141,
    INSTANTOS_SYS_NET_SET_CONFIG = 142,
    INSTANTOS_SYS_KERNEL_LOG = 151,
};

#define NET_CONFIG_MAX_DNS 3
#define NET_CONFIG_DOMAIN_SIZE 64

enum { NET_CONFIG_UP = 1u << 0, NET_CONFIG_DHCP = 1u << 1 };
enum { NET_CONFIG_STATIC = 0, NET_CONFIG_DHCP_START = 1, NET_CONFIG_DHCP_RENEW = 2, NET_CONFIG_DOWN = 3 };

// Addresses in network byte order, as in sockaddr_in.
struct net_config {
    uint32_t flags;
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns[NET_CONFIG_MAX_DNS];
    uint32_t dns_count;
    uint32_t dhcp_server;
    uint32_t lease_seconds;
    uint32_t lease_remaining;
    uint32_t ntp_server;
    uint64_t generation;
    char domain[NET_CONFIG_DOMAIN_SIZE];
    uint32_t link_up;
    char device[16];
};

// num in rax, arguments in rbx, r10, rdx, r8, r9; -errno on failure.
static inline long instantos_syscall(long n, long a, long b, long c) {
    long r;
    register long rbx __asm__("rbx") = a;
    register long r10 __asm__("r10") = b;
    register long rdx __asm__("rdx") = c;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "r"(rbx), "r"(r10), "r"(rdx) : "rcx", "r11", "memory");
    return r;
}
