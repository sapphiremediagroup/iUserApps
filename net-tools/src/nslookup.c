// nslookup / host: ask a DNS server directly (UDP, falling back to TCP for a
// truncated answer). The server is the argument or the first nameserver in
// /etc/resolv.conf. Installed under both names; argv[0] picks the output style.
//
//   nslookup [-type=T] [-timeout=S] name [server]
//   host [-t T] [-W S] name [server]
// T is A, AAAA, CNAME, MX, NS, PTR, SOA, TXT or ANY. An IPv4 address as the
// name asks for its PTR record.
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { T_A = 1, T_NS = 2, T_CNAME = 5, T_SOA = 6, T_PTR = 12, T_MX = 15, T_TXT = 16, T_AAAA = 28, T_ANY = 255 };

static const struct { const char* name; int type; } kTypes[] = {
    { "A", T_A }, { "NS", T_NS }, { "CNAME", T_CNAME }, { "SOA", T_SOA }, { "PTR", T_PTR },
    { "MX", T_MX }, { "TXT", T_TXT }, { "AAAA", T_AAAA }, { "ANY", T_ANY },
};

static int host_style;

static int type_from_name(const char* s) {
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
        if (strcasecmp(s, kTypes[i].name) == 0) return kTypes[i].type;
    }
    return -1;
}

static int default_server(struct in_addr* out) {
    FILE* f = fopen("/etc/resolv.conf", "r");
    if (!f) return -1;
    char line[256];
    int found = -1;
    while (found < 0 && fgets(line, sizeof(line), f)) {
        char addr[64];
        if (sscanf(line, " nameserver %63s", addr) == 1 && inet_pton(AF_INET, addr, out) == 1) {
            found = 0;
        }
    }
    fclose(f);
    return found;
}

// Append name as DNS labels; returns the new length or -1.
static int put_name(unsigned char* q, int n, const char* name) {
    const char* p = name;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + 1 + (int)len > 255) return -1;
        q[n++] = (unsigned char)len;
        memcpy(q + n, p, len);
        n += (int)len;
        p += len;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    return n;
}

// Read a possibly compressed name at *off into out (dotted); advances *off.
static int get_name(const unsigned char* m, int len, int* off, char* out, int cap) {
    int pos = *off, jumped = 0, o = 0, hops = 0;
    for (;;) {
        if (pos >= len) return -1;
        unsigned c = m[pos];
        if (c == 0) {
            if (!jumped) *off = pos + 1;
            break;
        }
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 32) return -1;
            if (!jumped) *off = pos + 2;
            pos = ((c & 0x3F) << 8) | m[pos + 1];
            jumped = 1;
            continue;
        }
        pos++;
        if (pos + (int)c > len || o + (int)c + 2 > cap) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, m + pos, c);
        o += (int)c;
        pos += (int)c;
    }
    if (o == 0) out[o++] = '.';
    out[o] = 0;
    return 0;
}

static int exchange_udp(struct sockaddr_in* server, const unsigned char* q, int qlen,
                        unsigned char* r, int rcap, double timeout) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int got = -1;
    for (int attempt = 0; attempt < 3 && got < 0; attempt++) {
        if (sendto(fd, q, qlen, 0, (struct sockaddr*)server, sizeof(*server)) < 0) break;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            int left = (int)(timeout * 1000) - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
            struct pollfd p = { fd, POLLIN, 0 };
            if (left <= 0 || poll(&p, 1, left) <= 0) break;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(fd, r, rcap, 0, (struct sockaddr*)&from, &fl);
            // The answer must come from the server and carry our ID.
            if (n >= 12 && from.sin_addr.s_addr == server->sin_addr.s_addr && r[0] == q[0] && r[1] == q[1]) {
                got = n;
                break;
            }
        }
    }
    close(fd);
    return got;
}

static int read_all(int fd, unsigned char* buf, int n) {
    int got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (int)r;
    }
    return got;
}

static int exchange_tcp(struct sockaddr_in* server, const unsigned char* q, int qlen,
                        unsigned char* r, int rcap, double timeout) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { (long)timeout, (long)((timeout - (long)timeout) * 1e6) };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int got = -1;
    unsigned char len[2] = { (unsigned char)(qlen >> 8), (unsigned char)qlen };
    if (connect(fd, (struct sockaddr*)server, sizeof(*server)) == 0 &&
        send(fd, len, 2, 0) == 2 && send(fd, q, qlen, 0) == qlen && read_all(fd, len, 2) == 2) {
        int n = (len[0] << 8) | len[1];
        if (n <= rcap && read_all(fd, r, n) == n) got = n;
    }
    close(fd);
    return got;
}

static const char* type_name(int type) {
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
        if (kTypes[i].type == type) return kTypes[i].name;
    }
    return "?";
}

// Print one answer record; returns 1 if it was printed.
static int print_record(const unsigned char* m, int len, int off, int type, int rdlen, const char* owner) {
    char name[512];
    int p = off;
    switch (type) {
    case T_A: {
        char ip[INET_ADDRSTRLEN];
        if (rdlen != 4) return 0;
        inet_ntop(AF_INET, m + off, ip, sizeof(ip));
        if (host_style) printf("%s has address %s\n", owner, ip);
        else printf("Name:\t%s\nAddress: %s\n", owner, ip);
        return 1;
    }
    case T_AAAA: {
        char ip[INET6_ADDRSTRLEN];
        if (rdlen != 16) return 0;
        inet_ntop(AF_INET6, m + off, ip, sizeof(ip));
        if (host_style) printf("%s has IPv6 address %s\n", owner, ip);
        else printf("Name:\t%s\nAddress: %s\n", owner, ip);
        return 1;
    }
    case T_CNAME: case T_NS: case T_PTR:
        if (get_name(m, len, &p, name, sizeof(name)) < 0) return 0;
        if (type == T_CNAME) printf(host_style ? "%s is an alias for %s.\n" : "%s\tcanonical name = %s.\n", owner, name);
        else if (type == T_NS) printf(host_style ? "%s name server %s.\n" : "%s\tnameserver = %s.\n", owner, name);
        else printf(host_style ? "%s domain name pointer %s.\n" : "%s\tname = %s.\n", owner, name);
        return 1;
    case T_MX: {
        if (rdlen < 3) return 0;
        int pref = (m[off] << 8) | m[off + 1];
        p = off + 2;
        if (get_name(m, len, &p, name, sizeof(name)) < 0) return 0;
        printf(host_style ? "%s mail is handled by %d %s.\n" : "%s\tmail exchanger = %d %s.\n", owner, pref, name);
        return 1;
    }
    case T_TXT: {
        printf(host_style ? "%s descriptive text " : "%s\ttext = ", owner);
        int q = off;
        while (q < off + rdlen) {
            int l = m[q++];
            if (q + l > off + rdlen) break;
            printf("\"%.*s\"%s", l, (const char*)m + q, q + l < off + rdlen ? " " : "");
            q += l;
        }
        printf("\n");
        return 1;
    }
    case T_SOA: {
        char mail[512];
        if (get_name(m, len, &p, name, sizeof(name)) < 0 || get_name(m, len, &p, mail, sizeof(mail)) < 0 ||
            p + 20 > len) return 0;
        unsigned serial = ((unsigned)m[p] << 24) | (m[p + 1] << 16) | (m[p + 2] << 8) | m[p + 3];
        printf(host_style ? "%s has SOA record %s. %s. %u\n" : "%s\n\torigin = %s\n\tmail addr = %s\n\tserial = %u\n",
               owner, name, mail, serial);
        return 1;
    }
    default:
        printf("%s\ttype %d, %d bytes\n", owner, type, rdlen);
        return 1;
    }
}

static void usage(void) {
    if (host_style) fprintf(stderr, "usage: host [-t type] [-W seconds] name [server]\n");
    else fprintf(stderr, "usage: nslookup [-type=T] [-timeout=S] name [server]\n");
    exit(2);
}

int main(int argc, char** argv) {
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    host_style = strcmp(prog, "host") == 0;

    int type = -1;
    double timeout = 2.0;
    const char* name = NULL;
    const char* server_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (host_style && strcmp(a, "-t") == 0 && i + 1 < argc) {
            type = type_from_name(argv[++i]);
            if (type < 0) usage();
        } else if (host_style && strcmp(a, "-W") == 0 && i + 1 < argc) {
            timeout = strtod(argv[++i], NULL);
        } else if (!host_style && (strncmp(a, "-type=", 6) == 0 || strncmp(a, "-query=", 7) == 0)) {
            type = type_from_name(strchr(a, '=') + 1);
            if (type < 0) usage();
        } else if (!host_style && strncmp(a, "-timeout=", 9) == 0) {
            timeout = strtod(a + 9, NULL);
        } else if (a[0] == '-') {
            usage();
        } else if (!name) {
            name = a;
        } else if (!server_arg) {
            server_arg = a;
        } else {
            usage();
        }
    }
    if (!name) usage();
    if (timeout <= 0) timeout = 2.0;

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(53);
    if (server_arg ? inet_pton(AF_INET, server_arg, &server.sin_addr) != 1 : default_server(&server.sin_addr) != 0) {
        fprintf(stderr, "%s: %s\n", prog, server_arg ? "the server must be an IPv4 address" : "no nameserver in /etc/resolv.conf");
        return 2;
    }
    char server_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server.sin_addr, server_ip, sizeof(server_ip));

    // An address as the name: its PTR record, d.c.b.a.in-addr.arpa.
    char qname[300];
    struct in_addr literal;
    if (inet_pton(AF_INET, name, &literal) == 1 && (type < 0 || type == T_PTR)) {
        const unsigned char* b = (const unsigned char*)&literal;
        snprintf(qname, sizeof(qname), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
        type = T_PTR;
    } else {
        snprintf(qname, sizeof(qname), "%s", name);
        size_t l = strlen(qname);
        if (l > 1 && qname[l - 1] == '.') qname[l - 1] = 0;
    }
    // host looks up A, AAAA and MX by default; nslookup A.
    int types[3] = { type < 0 ? T_A : type, 0, 0 };
    int ntypes = 1;
    if (type < 0 && host_style) {
        types[1] = T_AAAA;
        types[2] = T_MX;
        ntypes = 3;
    }

    if (!host_style) printf("Server:\t\t%s\nAddress:\t%s#53\n\n", server_ip, server_ip);
    int printed = 0, header = 0, status = 0;
    for (int t = 0; t < ntypes; t++) {
        unsigned char q[512], r[65535];
        uint16_t id;
        getentropy(&id, sizeof(id));
        memset(q, 0, 12);
        q[0] = (unsigned char)(id >> 8);
        q[1] = (unsigned char)id;
        q[2] = 0x01;   // RD
        q[5] = 1;      // one question
        int n = put_name(q, 12, qname);
        if (n < 0) {
            fprintf(stderr, "%s: bad name: %s\n", prog, name);
            return 2;
        }
        q[n++] = 0; q[n++] = (unsigned char)types[t];
        q[n++] = 0; q[n++] = 1;   // class IN
        int len = exchange_udp(&server, q, n, r, sizeof(r), timeout);
        if (len >= 12 && (r[2] & 0x02)) {   // TC: the whole answer over TCP
            int tl = exchange_tcp(&server, q, n, r, sizeof(r), timeout);
            if (tl >= 12) len = tl;
        }
        if (len < 12) {
            if (host_style) printf(";; connection timed out; no servers could be reached\n");
            else printf(";; connection timed out; no servers could be reached\n");
            return 1;
        }
        int rcode = r[3] & 0x0F;
        if (rcode != 0) {
            static const char* kCodes[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED" };
            const char* code = rcode < 6 ? kCodes[rcode] : "error";
            if (host_style) printf("Host %s not found: %d(%s)\n", name, rcode, code);
            else printf("** server can't find %s: %s\n", name, code);
            return 1;
        }
        int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
        int off = 12;
        char owner[512];
        for (int i = 0; i < qd; i++) {
            if (get_name(r, len, &off, owner, sizeof(owner)) < 0) return 1;
            off += 4;
        }
        if (!host_style && !header && an > 0) {
            printf((r[2] & 0x04) ? "Authoritative answer:\n" : "Non-authoritative answer:\n");
            header = 1;
        }
        for (int i = 0; i < an; i++) {
            if (get_name(r, len, &off, owner, sizeof(owner)) < 0 || off + 10 > len) return 1;
            int rtype = (r[off] << 8) | r[off + 1];
            int rdlen = (r[off + 8] << 8) | r[off + 9];
            off += 10;
            if (off + rdlen > len) return 1;
            // host prints a CNAME once, with the first type that brings it.
            if (!(host_style && rtype == T_CNAME && t > 0)) {
                printed += print_record(r, len, off, rtype, rdlen, owner);
            }
            off += rdlen;
        }
        if (an == 0 && types[t] != T_A && ntypes == 1) {
            printf(host_style ? "%s has no %s record\n" : "*** Can't find %s: No answer\n",
                   name, type_name(types[t]));
            status = 1;
        }
    }
    if (!printed && ntypes > 1) {
        printf("%s has no address\n", name);
        status = 1;
    } else if (!printed && !host_style && types[0] == T_A) {
        printf("*** Can't find %s: No answer\n", name);
        status = 1;
    }
    return status;
}
