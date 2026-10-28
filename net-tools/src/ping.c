// ping: ICMP echo through a ping socket (socket(AF_INET, SOCK_DGRAM,
// IPPROTO_ICMP)), so it needs no raw sockets or privileges. The kernel sets
// the identifier; replies are matched by sequence number.
//
//   ping [-c count] [-i interval] [-s size] [-W timeout] [-q] host
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 65499   // 65507 bytes of ICMP less the 8-byte header
#define MAX_TRACKED 65536

static volatile sig_atomic_t stop;
static double sent_at[MAX_TRACKED];   // ms, by sequence number; 0 = answered or unsent

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void on_interrupt(int sig) {
    (void)sig;
    stop = 1;
}

static void usage(void) {
    fprintf(stderr, "usage: ping [-c count] [-i interval] [-s size] [-W timeout] [-q] host\n");
    exit(2);
}

static int resolve(const char* host, struct sockaddr_in* out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return 0;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        fprintf(stderr, "ping: %s: %s\n", host, rc ? gai_strerror(rc) : "no address");
        return -1;
    }
    out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int main(int argc, char** argv) {
    long count = -1;
    double interval = 1.0, timeout = -1;
    long size = 56;
    int quiet = 0, opt;
    while ((opt = getopt(argc, argv, "c:i:s:W:qh")) != -1) {
        switch (opt) {
        case 'c': count = strtol(optarg, NULL, 10); if (count <= 0) usage(); break;
        case 'i': interval = strtod(optarg, NULL); if (interval < 0.01) interval = 0.01; break;
        case 's': size = strtol(optarg, NULL, 10); if (size < 0 || size > MAX_PAYLOAD) usage(); break;
        case 'W': timeout = strtod(optarg, NULL); break;
        case 'q': quiet = 1; break;
        default: usage();
        }
    }
    if (optind != argc - 1) {
        usage();
    }
    const char* host = argv[optind];
    struct sockaddr_in dest;
    if (resolve(host, &dest) != 0) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        fprintf(stderr, "ping: socket: %s\n", strerror(errno));
        return 2;
    }
    signal(SIGINT, on_interrupt);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dest.sin_addr, ip, sizeof(ip));
    printf("PING %s (%s) %ld(%ld) bytes of data.\n", host, ip, size, size + 28);
    fflush(stdout);

    static unsigned char out[8 + MAX_PAYLOAD], in[8 + MAX_PAYLOAD + 64];
    for (long i = 0; i < size; i++) {
        out[8 + i] = (unsigned char)(i + 8);
    }
    long transmitted = 0, received = 0, duplicates = 0, errors = 0;
    double min = 1e18, max = 0, sum = 0, sum2 = 0;
    const double start = now_ms();
    double next_send = start;
    // After the last request: the -W timeout, else twice the slowest reply
    // or 2 s, whichever is more (as iputils does).
    double give_up = 0;

    while (!stop) {
        double now = now_ms();
        if ((count < 0 || transmitted < count) && now >= next_send) {
            unsigned seq = (unsigned)(transmitted + 1) & 0xFFFF;
            out[0] = 8;   // echo request
            out[1] = 0;
            out[2] = out[3] = 0;   // checksum: the kernel's
            out[4] = out[5] = 0;   // identifier: the kernel's
            out[6] = (unsigned char)(seq >> 8);
            out[7] = (unsigned char)seq;
            if (sendto(fd, out, 8 + size, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
                fprintf(stderr, "ping: sendto: %s\n", strerror(errno));
                errors++;
            } else {
                sent_at[seq] = now;
            }
            transmitted++;
            next_send = now + interval * 1000.0;
            if (count >= 0 && transmitted == count) {
                double wait = timeout >= 0 ? timeout * 1000.0 : (max * 2 > 2000 ? max * 2 : 2000);
                give_up = now + wait;
            }
        }
        if (count >= 0 && transmitted == count && (received + errors >= count || now >= give_up)) {
            break;
        }

        double until = (count >= 0 && transmitted == count) ? give_up : next_send;
        int wait_ms = (int)(until - now_ms());
        if (wait_ms < 0) wait_ms = 0;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, wait_ms) <= 0 || !(p.revents & POLLIN)) {
            continue;
        }
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, in, sizeof(in), 0, (struct sockaddr*)&from, &fromlen);
        double t = now_ms();
        if (n < 8 || in[0] != 0) {
            continue;
        }
        unsigned seq = ((unsigned)in[6] << 8) | in[7];
        char from_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
        if (sent_at[seq] == 0) {
            duplicates++;
            if (!quiet) printf("%zd bytes from %s: icmp_seq=%u (DUP!)\n", n, from_ip, seq);
            continue;
        }
        double rtt = t - sent_at[seq];
        sent_at[seq] = 0;
        received++;
        if (rtt < min) min = rtt;
        if (rtt > max) max = rtt;
        sum += rtt;
        sum2 += rtt * rtt;
        if (!quiet) {
            printf("%zd bytes from %s: icmp_seq=%u time=%.3f ms\n", n, from_ip, seq, rtt);
            fflush(stdout);
        }
    }

    double elapsed = now_ms() - start;
    printf("\n--- %s ping statistics ---\n", host);
    printf("%ld packets transmitted, %ld received", transmitted, received);
    if (duplicates) printf(", +%ld duplicates", duplicates);
    if (errors) printf(", +%ld errors", errors);
    printf(", %ld%% packet loss, time %.0fms\n",
           transmitted ? (transmitted - received) * 100 / transmitted : 0, elapsed);
    if (received) {
        double avg = sum / received;
        double var = sum2 / received - avg * avg;
        double mdev = 0;
        if (var > 0) {   // a Newton square root: no libm needed
            mdev = var;
            for (int i = 0; i < 30; i++) mdev = 0.5 * (mdev + var / mdev);
        }
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", min, avg, max, mdev);
    }
    close(fd);
    return received ? 0 : 1;
}
