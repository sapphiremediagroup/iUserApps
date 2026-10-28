// sntp: asks an NTP server for the time (SNTPv4, RFC 4330) and prints how
// far the local clock is off; -s also sets the clock (root only). The server
// is the argument, else the one DHCP handed out, else pool.ntp.org.
//
//   sntp [-s] [-q] [-W timeout] [-r retries] [server]
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "instantos_net.h"

#define NTP_PORT 123
#define NTP_UNIX_OFFSET 2208988800ULL   // 1900-01-01 to 1970-01-01, in seconds
#define DEFAULT_SERVER "pool.ntp.org"

struct ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint32_t reference[2];
    uint32_t originate[2];
    uint32_t receive[2];
    uint32_t transmit[2];
};

static void usage(void) {
    fprintf(stderr, "usage: sntp [-s] [-q] [-W timeout] [-r retries] [server]\n");
    exit(2);
}

static double now_unix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// NTP timestamps are seconds since 1900 plus a 32-bit fraction. Seconds below
// 2^31 belong to era 1 (from 2036).
static double ntp_to_unix(const uint32_t t[2]) {
    uint64_t seconds = ntohl(t[0]);
    if (seconds < 0x80000000ULL) {
        seconds += 0x100000000ULL;
    }
    return (double)(seconds - NTP_UNIX_OFFSET) + ntohl(t[1]) / 4294967296.0;
}

static void unix_to_ntp(double unix_time, uint32_t t[2]) {
    double seconds = unix_time + NTP_UNIX_OFFSET;
    uint64_t whole = (uint64_t)seconds;
    t[0] = htonl((uint32_t)whole);
    t[1] = htonl((uint32_t)((seconds - whole) * 4294967296.0));
}

static int resolve(const char* host, struct sockaddr_in* out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(NTP_PORT);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return 0;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        fprintf(stderr, "sntp: %s: %s\n", host, rc ? gai_strerror(rc) : "no address");
        return -1;
    }
    out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

// The DHCP-provided NTP server, or NULL.
static const char* dhcp_server(char* buf, size_t size) {
    struct net_config config;
    memset(&config, 0, sizeof(config));
    if (instantos_syscall(INSTANTOS_SYS_NET_GET_CONFIG, (long)&config, 0, 0) != 0 ||
        !(config.flags & NET_CONFIG_UP) || config.ntp_server == 0) {
        return NULL;
    }
    struct in_addr a = { config.ntp_server };
    return inet_ntop(AF_INET, &a, buf, size);
}

// One exchange. Returns 1 on an answer (offset and delay in seconds), 0 on
// a timeout, -1 on an error already reported.
static int query(int fd, double timeout, double* offset, double* delay, int* stratum) {
    struct ntp_packet request;
    memset(&request, 0, sizeof(request));
    request.li_vn_mode = (0 << 6) | (4 << 3) | 3;   // no leap warning, v4, client
    const double t1 = now_unix();
    unix_to_ntp(t1, request.transmit);
    if (send(fd, &request, sizeof(request), 0) < 0) {
        fprintf(stderr, "sntp: send: %s\n", strerror(errno));
        return -1;
    }
    const double deadline = t1 + timeout;
    for (;;) {
        double left = deadline - now_unix();
        if (left <= 0) {
            return 0;
        }
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ready = poll(&pfd, 1, (int)(left * 1000) + 1);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) {
                fprintf(stderr, "sntp: poll: %s\n", strerror(errno));
                return -1;
            }
            continue;
        }
        struct ntp_packet reply;
        ssize_t n = recv(fd, &reply, sizeof(reply), 0);
        const double t4 = now_unix();
        if (n < 0) {
            fprintf(stderr, "sntp: recv: %s\n", strerror(errno));
            return -1;
        }
        // A reply to this request: server mode, our transmit time echoed back.
        if (n < (ssize_t)sizeof(reply) || (reply.li_vn_mode & 7) != 4 ||
            reply.originate[0] != request.transmit[0] || reply.originate[1] != request.transmit[1]) {
            continue;
        }
        if (reply.stratum == 0 || reply.stratum > 15) {
            char code[5] = { 0 };
            memcpy(code, &reply.reference_id, 4);
            fprintf(stderr, "sntp: server refused (stratum %u, %s)\n", reply.stratum, code);
            return -1;
        }
        if ((reply.li_vn_mode >> 6) == 3) {
            fprintf(stderr, "sntp: server clock is not synchronized\n");
            return -1;
        }
        const double t2 = ntp_to_unix(reply.receive), t3 = ntp_to_unix(reply.transmit);
        *offset = ((t2 - t1) + (t3 - t4)) / 2;
        *delay = (t4 - t1) - (t3 - t2);
        *stratum = reply.stratum;
        return 1;
    }
}

int main(int argc, char** argv) {
    int set = 0, quiet = 0, retries = 3, opt;
    double timeout = 2;
    while ((opt = getopt(argc, argv, "sqW:r:h")) != -1) {
        switch (opt) {
        case 's': set = 1; break;
        case 'q': quiet = 1; break;
        case 'W': timeout = strtod(optarg, NULL); if (timeout <= 0) usage(); break;
        case 'r': retries = (int)strtol(optarg, NULL, 10); if (retries < 1) usage(); break;
        default: usage();
        }
    }
    if (optind < argc - 1) {
        usage();
    }
    char dhcp[16];
    const char* server = optind < argc ? argv[optind] : dhcp_server(dhcp, sizeof(dhcp));
    if (!server) {
        server = DEFAULT_SERVER;
    }

    struct sockaddr_in addr;
    if (resolve(server, &addr) != 0) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "sntp: %s: %s\n", server, strerror(errno));
        return 1;
    }
    char ip[16];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    double offset = 0, delay = 0;
    int stratum = 0, got = 0;
    for (int i = 0; i < retries && got == 0; i++) {
        got = query(fd, timeout, &offset, &delay, &stratum);
    }
    close(fd);
    if (got <= 0) {
        if (got == 0) {
            fprintf(stderr, "sntp: no answer from %s (%s)\n", server, ip);
        }
        return 1;
    }
    if (!quiet) {
        // The sign by hand: mlibc's printf ignores '+' for floats.
        printf("%s (%s): stratum %d, offset %c%.6f s, delay %.6f s\n", server, ip, stratum,
               offset < 0 ? '-' : '+', offset < 0 ? -offset : offset, delay);
    }
    if (set) {
        double target = now_unix() + offset;
        struct timespec ts = { (time_t)target, (long)((target - (time_t)target) * 1e9) };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            fprintf(stderr, "sntp: cannot set the clock: %s\n", strerror(errno));
            return 1;
        }
        if (!quiet) {
            printf("clock stepped by %c%.6f s\n", offset < 0 ? '-' : '+', offset < 0 ? -offset : offset);
        }
    }
    return 0;
}
