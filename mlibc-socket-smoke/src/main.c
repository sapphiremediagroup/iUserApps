// mlibc socket smoke test.
//
// Exercises the InstantOS mlibc socket sysdeps end-to-end using the standard
// mlibc socket API (socket/bind/listen/connect/accept/send/recv).  The test
// uses a 127.0.0.1 loopback connection, which the kernel services entirely
// in-kernel (no NIC required), so it validates the sysdep plumbing in
// isolation from the network stack/driver layer.
//
// Success is reported on the serial port with the marker
// "mlibc-socket: serial-done"; any failure prints "mlibc-socket: FAIL ...".

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static unsigned long instant_syscall2(unsigned long number, unsigned long arg1, unsigned long arg2) {
    unsigned long result;
    register unsigned long rax asm("rax") = number;
    register unsigned long rbx asm("rbx") = arg1;
    register unsigned long r10 asm("r10") = arg2;
    asm volatile("syscall" : "=a"(result) : "a"(rax), "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

static void serial_write(const char* text) {
    unsigned long length = 0;
    while (text[length]) {
        length++;
    }
    instant_syscall2(88, (unsigned long)text, length);
}

static void fail(const char* what) {
    serial_write("mlibc-socket: FAIL ");
    serial_write(what);
    serial_write("\n");
}

int main(void) {
    serial_write("mlibc-socket: serial-start\n");

    const unsigned short port = 8080;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(0x7f000001); // 127.0.0.1

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fail("socket(listener)");
        return 1;
    }

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fail("bind");
        return 1;
    }

    if (listen(listener, 4) != 0) {
        fail("listen");
        return 1;
    }
    serial_write("mlibc-socket: listening\n");

    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        fail("socket(client)");
        return 1;
    }

    if (connect(client, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fail("connect");
        return 1;
    }
    serial_write("mlibc-socket: connected\n");

    int server = accept(listener, NULL, NULL);
    if (server < 0) {
        fail("accept");
        return 1;
    }
    serial_write("mlibc-socket: accepted\n");

    const char* msg = "hello-socket";
    ssize_t sent = send(client, msg, strlen(msg), 0);
    if (sent != (ssize_t)strlen(msg)) {
        fail("send length mismatch");
        return 1;
    }

    char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    ssize_t received = recv(server, buffer, sizeof(buffer) - 1, 0);
    if (received != (ssize_t)strlen(msg)) {
        fail("recv length mismatch");
        return 1;
    }
    if (strcmp(buffer, msg) != 0) {
        fail("recv payload mismatch");
        return 1;
    }

    serial_write("mlibc-socket: roundtrip-ok payload=");
    serial_write(buffer);
    serial_write("\n");

    shutdown(client, SHUT_RDWR);
    close(client);
    close(server);
    close(listener);

    serial_write("mlibc-socket: serial-done\n");
    return 0;
}
