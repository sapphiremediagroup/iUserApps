// netcfg: show or change the network interface configuration.
//
//   netcfg                                  show it
//   netcfg dhcp                             drop the address, get a new lease
//   netcfg renew                            renew the current lease
//   netcfg static ADDR/PREFIX [GW [DNS...]] set an address by hand (DHCP off)
//   netcfg down                             no address
// Changing it needs root. dhcp, renew and static wait (up to -w seconds,
// default 10) until the kernel reports the new configuration.
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "instantos_net.h"

static int get_config(struct net_config* c, uint64_t known, long timeout_ms) {
    memset(c, 0, sizeof(*c));
    long r = instantos_syscall(INSTANTOS_SYS_NET_GET_CONFIG, (long)c, (long)known, timeout_ms);
    if (r < 0) {
        fprintf(stderr, "netcfg: NetGetConfig: %s\n", strerror((int)-r));
        return -1;
    }
    return 0;
}

static const char* ip(uint32_t a, char* buf) {
    inet_ntop(AF_INET, &a, buf, INET_ADDRSTRLEN);
    return buf;
}

static int prefix_length(uint32_t mask_net) {
    uint32_t m = ntohl(mask_net);
    int n = 0;
    while (m & 0x80000000u) {
        n++;
        m <<= 1;
    }
    return n;
}

static void show(void) {
    struct net_config c;
    if (get_config(&c, 0, 0) != 0) exit(1);
    unsigned char mac[6] = { 0 };
    long have_nic = instantos_syscall(INSTANTOS_SYS_NET_GET_MAC, (long)mac, 0, 0);
    long link = instantos_syscall(INSTANTOS_SYS_NET_LINK_STATUS, 0, 0, 0);
    char a[INET_ADDRSTRLEN];
    c.device[sizeof(c.device) - 1] = '\0';
    printf("eth0: %s, link %s\n", have_nic != 0 ? "no NIC" : c.device[0] ? c.device : "present",
           link == 1 ? "up" : "down");
    if (have_nic == 0) {
        printf("  ether    %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (!(c.flags & NET_CONFIG_UP)) {
        printf("  inet     none (%s)\n", "no address: waiting for DHCP or down");
        return;
    }
    printf("  inet     %s/%d (%s)\n", ip(c.address, a), prefix_length(c.netmask),
           (c.flags & NET_CONFIG_DHCP) ? "dhcp" : "static");
    printf("  netmask  %s\n", ip(c.netmask, a));
    printf("  gateway  %s\n", c.gateway ? ip(c.gateway, a) : "none");
    for (uint32_t i = 0; i < c.dns_count && i < NET_CONFIG_MAX_DNS; i++) {
        printf("  dns      %s\n", ip(c.dns[i], a));
    }
    if (c.ntp_server) {
        printf("  ntp      %s\n", ip(c.ntp_server, a));
    }
    if (c.domain[0]) {
        printf("  domain   %.*s\n", NET_CONFIG_DOMAIN_SIZE, c.domain);
    }
    if (c.flags & NET_CONFIG_DHCP) {
        printf("  server   %s\n", ip(c.dhcp_server, a));
        if (c.lease_seconds == 0xFFFFFFFFu) {
            printf("  lease    infinite\n");
        } else {
            printf("  lease    %u s, %u s left\n", c.lease_seconds, c.lease_remaining);
        }
    }
}

static int set_config(struct net_config* c, int mode) {
    long r = instantos_syscall(INSTANTOS_SYS_NET_SET_CONFIG, (long)c, mode, 0);
    if (r < 0) {
        fprintf(stderr, "netcfg: %s\n", r == -EACCES ? "permission denied (root only)" : strerror((int)-r));
        return -1;
    }
    return 0;
}

// Wait until the configuration moves past `generation` and matches `want`
// (flags), up to `seconds`.
static int wait_for(uint64_t generation, uint32_t want, double seconds) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct net_config c;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long left = (long)(seconds * 1000) - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) {
            fprintf(stderr, "netcfg: no answer within %.0f s\n", seconds);
            return -1;
        }
        if (get_config(&c, generation, left) != 0) return -1;
        if (c.generation != generation) {
            generation = c.generation;
            if (c.flags == want) return 0;
        }
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: netcfg [-w seconds] [dhcp | renew | down | static ADDR/PREFIX [GATEWAY [DNS...]]]\n");
    exit(2);
}

int main(int argc, char** argv) {
    double wait = 10;
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-w") == 0) {
        wait = strtod(argv[i + 1], NULL);
        i += 2;
    }
    if (i >= argc) {
        show();
        return 0;
    }
    const char* cmd = argv[i++];
    struct net_config before;
    if (get_config(&before, 0, 0) != 0) return 1;

    if (strcmp(cmd, "dhcp") == 0 && i == argc) {
        if (set_config(NULL, NET_CONFIG_DHCP_START) != 0) return 1;
        if (wait_for(before.generation, NET_CONFIG_UP | NET_CONFIG_DHCP, wait) != 0) return 1;
    } else if (strcmp(cmd, "renew") == 0 && i == argc) {
        if (!(before.flags & NET_CONFIG_DHCP)) {
            fprintf(stderr, "netcfg: no DHCP lease to renew\n");
            return 1;
        }
        if (set_config(NULL, NET_CONFIG_DHCP_RENEW) != 0) return 1;
        if (wait_for(before.generation, NET_CONFIG_UP | NET_CONFIG_DHCP, wait) != 0) return 1;
    } else if (strcmp(cmd, "down") == 0 && i == argc) {
        if (set_config(NULL, NET_CONFIG_DOWN) != 0) return 1;
    } else if (strcmp(cmd, "static") == 0 && i < argc) {
        struct net_config c;
        memset(&c, 0, sizeof(c));
        char addr[32];
        snprintf(addr, sizeof(addr), "%s", argv[i++]);
        int prefix = 24;
        char* slash = strchr(addr, '/');
        if (slash) {
            *slash = 0;
            prefix = atoi(slash + 1);
        }
        if (inet_pton(AF_INET, addr, &c.address) != 1 || prefix < 1 || prefix > 32) usage();
        c.netmask = htonl(prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
        if (i < argc && inet_pton(AF_INET, argv[i++], &c.gateway) != 1) usage();
        while (i < argc) {
            if (c.dns_count == NET_CONFIG_MAX_DNS || inet_pton(AF_INET, argv[i++], &c.dns[c.dns_count]) != 1) usage();
            c.dns_count++;
        }
        if (set_config(&c, NET_CONFIG_STATIC) != 0) return 1;
    } else {
        usage();
    }
    show();
    return 0;
}
