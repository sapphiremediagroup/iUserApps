// mlibc DNS resolution smoke test.
//
// Exercises mlibc's built-in stub resolver end-to-end: getaddrinfo() for a
// real hostname, which triggers a UDP DNS query to the nameserver configured
// in /etc/resolv.conf (QEMU user-net forwarder 10.0.2.3).  Requires the
// network-manager packet pump to be running so incoming UDP replies are
// delivered.
//
// Success marker on serial: "mlibc-dns: serial-done"; failures print
// "mlibc-dns: FAIL ...".

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static unsigned long instant_syscall0(unsigned long number) {
    unsigned long result;
    register unsigned long rax asm("rax") = number;
    asm volatile("syscall" : "=a"(result) : "a"(rax) : "rcx", "r11", "memory");
    return result;
}

static unsigned long instant_syscall2(unsigned long number, unsigned long arg1, unsigned long arg2) {
    unsigned long result;
    register unsigned long rax asm("rax") = number;
    register unsigned long rbx asm("rbx") = arg1;
    register unsigned long r10 asm("r10") = arg2;
    asm volatile("syscall" : "=a"(result) : "a"(rax), "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

// Syscall numbers (ordinals of enum class SyscallNumber in the kernel).
#define SYS_SLEEP 15
#define SYS_SERIAL_WRITE 88
#define SYS_NET_PROCESS_PACKETS 72

static void serial_write(const char* text) {
    unsigned long length = 0;
    while (text[length]) length++;
    instant_syscall2(SYS_SERIAL_WRITE, (unsigned long)text, length);
}

// Drive the NIC RX/TX pump.  While getaddrinfo() blocks in poll() waiting for a
// DNS reply, nothing else processes incoming packets, so the test forks a child
// that continuously calls NetProcessPackets (the same job network-manager does
// in a normal boot).  This keeps the test self-contained.
static void packet_pump_forever(void) {
    for (;;) {
        instant_syscall0(SYS_NET_PROCESS_PACKETS);
        instant_syscall2(SYS_SLEEP, 5, 0);
    }
}


static void resolve(const char* host) {
    serial_write("mlibc-dns: resolving ");
    serial_write(host);
    serial_write("\n");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // IPv4 only
    hints.ai_socktype = SOCK_STREAM;  // explicit hints to avoid AI_ADDRCONFIG

    struct addrinfo* result = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "mlibc-dns: FAIL getaddrinfo(%s) rc=%d\n", host, rc);
        serial_write(buf);
        return;
    }

    int printed = 0;
    for (struct addrinfo* ai = result; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        struct sockaddr_in* sin = (struct sockaddr_in*)ai->ai_addr;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        char buf[128];
        snprintf(buf, sizeof(buf), "mlibc-dns: %s -> %s\n", host, ip);
        serial_write(buf);
        printed++;
    }
    if (!printed) {
        serial_write("mlibc-dns: FAIL no A records\n");
    }
    freeaddrinfo(result);
}

int main(void) {
    serial_write("mlibc-dns: serial-start\n");

    pid_t pump = fork();
    if (pump == 0) {
        packet_pump_forever();
        _exit(0);
    }

    resolve("example.com");
    resolve("one.one.one.one");
    serial_write("mlibc-dns: serial-done\n");
    return 0;
}
