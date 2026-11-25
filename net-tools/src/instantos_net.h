// What the network tools need from InstantOS beyond POSIX: the interface
// configuration syscalls (NetGetConfig/NetSetConfig) and their NetConfig
// struct, mirrored from include/cpu/syscall/syscall.hpp, KernelLog (dmesg) and
// the driver modules (lsmod, modload).
#pragma once

#include <stdint.h>

enum {
    INSTANTOS_SYS_NET_GET_MAC = 67,
    INSTANTOS_SYS_NET_LINK_STATUS = 70,
    INSTANTOS_SYS_NET_GET_CONFIG = 141,
    INSTANTOS_SYS_NET_SET_CONFIG = 142,
    INSTANTOS_SYS_KERNEL_LOG = 151,
    INSTANTOS_SYS_MODULE_LIST = 163,
    INSTANTOS_SYS_MODULE_LOAD = 164,
    INSTANTOS_SYS_PARTITION_LIST = 165,
    INSTANTOS_SYS_PARTITION_SET_TYPE = 166,
    INSTANTOS_SYS_PARTITION_MOUNT = 167,
};

// PartitionList's records (instant-install), mirrored from struct
// PartitionRecord in include/cpu/syscall/syscall.hpp.
struct partition_record {
    uint32_t disk;
    uint32_t index;
    uint64_t start_byte;
    uint64_t length_byte;
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    char name[40];
    uint32_t flags;
    uint32_t reserved;
};
enum { PARTITION_INSTANTOS = 1u << 0, PARTITION_ESP = 1u << 1, PARTITION_MOUNTED = 1u << 2 };

// ModuleList's records (lsmod), mirrored from struct ModuleInfo in
// include/cpu/syscall/syscall.hpp.
struct module_info {
    char name[32];
    char version[16];
    char description[80];
    char source[96];
    uint64_t base;
    uint32_t image_size;
    uint32_t devices;
    uint32_t relocations;
    uint32_t flags;
};
enum { MODULE_INIT_FAILED = 1u << 0, MODULE_AT_BOOT = 1u << 1 };

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
