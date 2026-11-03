// unix-test: AF_UNIX sockets as a window system uses them. Stream and
// seqpacket socketpairs, a named and an abstract server across fork, poll,
// non-blocking calls, and SCM_RIGHTS: pipe ends and files passed within a
// process and from a child, several at once, MSG_CTRUNC, and (when the GPU's
// render node opens) a dma-buf whose pages a child writes and the parent
// reads.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int checks;

static void check(int ok, const char* what) {
    checks++;
    printf("unix-test: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static void checkf(int ok, const char* what, long detail) {
    checks++;
    printf("unix-test: %s %s (%ld)\n", ok ? "ok  " : "FAIL", what, detail);
    if (!ok) {
        failures++;
    }
}

// One message with `count` fds.
static ssize_t send_fds(int sock, const void* data, size_t length, const int* fds, int count) {
    struct iovec iov = {(void*)data, length};
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (count > 0) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
        struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * count);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * count);
    }
    return sendmsg(sock, &msg, 0);
}

// Receives into buf; fds land in fds (room for `room`), *got their count.
static ssize_t recv_fds(int sock, void* buf, size_t length, int* fds, int room, int* got, int* flags) {
    struct iovec iov = {buf, length};
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_LEN(sizeof(int) * room);   // room for exactly `room` fds
    ssize_t n = recvmsg(sock, &msg, 0);
    *got = 0;
    if (n < 0) {
        return n;
    }
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + *got, CMSG_DATA(c), sizeof(int) * k);
            *got += k;
        }
    }
    if (flags) {
        *flags = msg.msg_flags;
    }
    return n;
}

static void test_stream_pair(void) {
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair(SOCK_STREAM)");
    char buf[64];
    check(write(sv[0], "hello", 5) == 5, "write() on a socket");
    memset(buf, 0, sizeof(buf));
    check(read(sv[1], buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0, "read() gets it on the other end");
    check(send(sv[1], "back", 4, 0) == 4 && recv(sv[0], buf, sizeof(buf), 0) == 4 && memcmp(buf, "back", 4) == 0,
          "and the other way (send/recv)");
    // Two writes are one stream.
    send(sv[0], "ab", 2, 0);
    send(sv[0], "cd", 2, 0);
    check(recv(sv[1], buf, sizeof(buf), 0) == 4 && memcmp(buf, "abcd", 4) == 0, "a stream joins writes");
    // Peek leaves the bytes.
    send(sv[0], "xyz", 3, 0);
    check(recv(sv[1], buf, 2, MSG_PEEK) == 2 && recv(sv[1], buf, sizeof(buf), 0) == 3 && memcmp(buf, "xyz", 3) == 0,
          "MSG_PEEK");
    int type = 0, domain = 0;
    socklen_t len = sizeof(type);
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len);
    len = sizeof(domain);
    getsockopt(sv[0], SOL_SOCKET, SO_DOMAIN, &domain, &len);
    check(type == SOCK_STREAM && domain == AF_UNIX, "SO_TYPE and SO_DOMAIN");
    struct ucred cred;
    len = sizeof(cred);
    check(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == getpid(),
          "SO_PEERCRED of a socketpair is this process");
    // Non-blocking: empty means EAGAIN; filling the peer's queue stops with EAGAIN.
    errno = 0;
    check(recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "empty + MSG_DONTWAIT: EAGAIN");
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    check((fcntl(sv[0], F_GETFL) & O_NONBLOCK) != 0, "O_NONBLOCK sticks (fcntl)");
    static char big[16384];
    long total = 0;
    for (int i = 0; i < 100; i++) {
        ssize_t n = send(sv[0], big, sizeof(big), 0);
        if (n < 0) {
            break;
        }
        total += n;
    }
    checkf(errno == EAGAIN && total >= 128 * 1024 && total <= 512 * 1024, "a full queue says EAGAIN", total);
    struct pollfd pfd = {sv[0], POLLOUT, 0};
    check(poll(&pfd, 1, 0) == 0, "poll: not writable while full");
    pfd.fd = sv[1];
    pfd.events = POLLIN;
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN), "poll: readable");
    long drained = 0;
    for (;;) {
        ssize_t n = recv(sv[1], big, sizeof(big), MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        drained += n;
    }
    checkf(drained == total, "every byte comes out", drained);
    pfd.fd = sv[0];
    pfd.events = POLLOUT;
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT), "poll: writable again");
    // shutdown(SHUT_WR): the peer reads EOF, the other direction still works.
    shutdown(sv[0], SHUT_WR);
    check(recv(sv[1], buf, sizeof(buf), 0) == 0, "SHUT_WR: the peer reads EOF");
    check(send(sv[1], "ok", 2, 0) == 2 && recv(sv[0], buf, sizeof(buf), MSG_DONTWAIT) == 2,
          "the other direction still works");
    // Close: EOF on read, EPIPE on write, POLLHUP.
    close(sv[0]);
    check(recv(sv[1], buf, sizeof(buf), 0) == 0, "the peer closed: EOF");
    errno = 0;
    check(send(sv[1], "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "the peer closed: EPIPE");
    pfd.fd = sv[1];
    pfd.events = POLLIN;
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP), "the peer closed: POLLHUP");
    close(sv[1]);
}

static void test_seqpacket(void) {
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair(SOCK_SEQPACKET)");
    send(sv[0], "abc", 3, 0);
    send(sv[0], "defgh", 5, 0);
    char buf[64];
    ssize_t a = recv(sv[1], buf, sizeof(buf), 0);
    ssize_t b = recv(sv[1], buf + 10, sizeof(buf) - 10, 0);
    check(a == 3 && b == 5 && memcmp(buf, "abc", 3) == 0 && memcmp(buf + 10, "defgh", 5) == 0,
          "records keep their boundaries");
    send(sv[0], "0123456789", 10, 0);
    int fds[1], got = 0, flags = 0;
    ssize_t n = recv_fds(sv[1], buf, 4, fds, 1, &got, &flags);
    check(n == 4 && (flags & MSG_TRUNC) && memcmp(buf, "0123", 4) == 0, "a short buffer: MSG_TRUNC, the rest dropped");
    errno = 0;
    check(recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "nothing left of it");
    close(sv[0]);
    close(sv[1]);
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair(SOCK_DGRAM)");
    send(sv[0], "p", 1, 0);
    send(sv[0], "qq", 2, 0);
    check(recv(sv[1], buf, sizeof(buf), 0) == 1 && recv(sv[1], buf, sizeof(buf), 0) == 2, "datagrams too");
    close(sv[0]);
    close(sv[1]);
}

static void test_named(void) {
    const char* path = "/tmp/unix-test.sock";
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    check(server >= 0, "socket(AF_UNIX, SOCK_STREAM)");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    unlink(path);
    check(bind(server, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind to a path");
    int second = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    check(bind(second, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno == EADDRINUSE, "a second bind: EADDRINUSE");
    errno = 0;
    check(connect(second, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno == ECONNREFUSED,
          "connect before listen: ECONNREFUSED");
    close(second);
    check(listen(server, 4) == 0, "listen");
    struct sockaddr_un missing = addr;
    strcpy(missing.sun_path, "/tmp/nobody-here.sock");
    int lost = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    check(connect(lost, (struct sockaddr*)&missing, sizeof(missing)) < 0 && errno == ENOENT, "an unbound path: ENOENT");
    close(lost);

    pid_t child = fork();
    if (child == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        // A relative path from /tmp reaches the same name.
        chdir("/tmp");
        struct sockaddr_un rel;
        memset(&rel, 0, sizeof(rel));
        rel.sun_family = AF_UNIX;
        strcpy(rel.sun_path, "unix-test.sock");
        if (connect(c, (struct sockaddr*)&rel, sizeof(rel)) != 0) {
            _exit(2);
        }
        char buf[32];
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n != 4 || memcmp(buf, "ping", 4) != 0) {
            _exit(3);
        }
        send(c, "pong", 4, 0);
        // Wait for the parent to close, then go.
        recv(c, buf, sizeof(buf), 0);
        _exit(0);
    }
    struct pollfd pfd = {server, POLLIN, 0};
    check(poll(&pfd, 1, 5000) == 1, "poll: the listener is readable when a client waits");
    struct sockaddr_un peer;
    socklen_t peerLength = sizeof(peer);
    int conn = accept(server, (struct sockaddr*)&peer, &peerLength);
    check(conn >= 0, "accept");
    struct ucred cred;
    socklen_t len = sizeof(cred);
    check(getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == child,
          "SO_PEERCRED: the client's pid");
    struct sockaddr_un name;
    socklen_t nameLength = sizeof(name);
    check(getsockname(conn, (struct sockaddr*)&name, &nameLength) == 0 && strcmp(name.sun_path, path) == 0,
          "getsockname of the accepted socket: the server's path");
    send(conn, "ping", 4, 0);
    char buf[32];
    check(recv(conn, buf, sizeof(buf), 0) == 4 && memcmp(buf, "pong", 4) == 0, "a round trip with a forked client");
    close(conn);
    int status = -1;
    waitpid(child, &status, 0);
    checkf(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the client exits cleanly", WEXITSTATUS(status));
    close(server);
    // The name goes with the socket.
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    check(bind(server, (struct sockaddr*)&addr, sizeof(addr)) == 0, "the path is free again after close");
    close(server);

    // An abstract name; a non-blocking client connects at once.
    struct sockaddr_un abs;
    memset(&abs, 0, sizeof(abs));
    abs.sun_family = AF_UNIX;
    memcpy(abs.sun_path, "\0instantos-unix-test", 20);
    socklen_t absLength = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 20);
    server = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    check(bind(server, (struct sockaddr*)&abs, absLength) == 0 && listen(server, 1) == 0, "an abstract seqpacket server");
    int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    check(connect(client, (struct sockaddr*)&abs, absLength) == 0, "connect to it");
    conn = accept(server, NULL, NULL);
    check(conn >= 0 && send(client, "hi", 2, 0) == 2 && recv(conn, buf, sizeof(buf), 0) == 2, "and talk");
    struct sockaddr_un got;
    socklen_t gotLength = sizeof(got);
    check(getpeername(client, (struct sockaddr*)&got, &gotLength) == 0 && gotLength == absLength &&
              memcmp(got.sun_path, abs.sun_path, 20) == 0,
          "getpeername: the abstract name");
    close(conn);
    close(client);
    close(server);
}

static void test_rights(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    // A pipe's write end goes over; writing through the copy reaches the pipe.
    int p[2];
    pipe(p);
    check(send_fds(sv[0], "w", 1, &p[1], 1) == 1, "sendmsg with SCM_RIGHTS (a pipe's write end)");
    char buf[64];
    int fds[8], got = 0, flags = 0;
    ssize_t n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, &flags);
    checkf(n == 1 && got == 1 && fds[0] != p[1], "recvmsg gets a new fd", got ? fds[0] : -1);
    if (got == 1) {
        write(fds[0], "through", 7);
        close(fds[0]);
    }
    memset(buf, 0, sizeof(buf));
    check(read(p[0], buf, sizeof(buf)) == 7 && memcmp(buf, "through", 7) == 0, "writing to it reaches the pipe");
    close(p[1]);
    check(read(p[0], buf, sizeof(buf)) == 0, "and when every copy is closed, the reader sees EOF");
    close(p[0]);

    // A file: the passed fd shares the open file (and its offset).
    int f = open("/tmp/unix-test.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    write(f, "file contents", 13);
    lseek(f, 0, SEEK_SET);
    send_fds(sv[0], "f", 1, &f, 1);
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, NULL);
    if (got == 1) {
        memset(buf, 0, sizeof(buf));
        ssize_t r = read(fds[0], buf, 4);
        check(r == 4 && memcmp(buf, "file", 4) == 0, "a passed file reads its contents");
        check(lseek(f, 0, SEEK_CUR) == 4, "and shares the offset with the sender's fd");
        close(fds[0]);
    } else {
        check(0, "a passed file arrives");
    }
    close(f);

    // Several at once, and the stream stops at bytes carrying fds.
    int a[2], b[2], c[2];
    pipe(a);
    pipe(b);
    pipe(c);
    int three[3] = {a[0], b[0], c[0]};
    send(sv[0], "ab", 2, 0);
    send_fds(sv[0], "cd", 2, three, 3);
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, NULL);
    check(n == 2 && got == 0 && memcmp(buf, "ab", 2) == 0, "a read stops before bytes that carry fds");
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, NULL);
    check(n == 2 && got == 3 && memcmp(buf, "cd", 2) == 0, "three fds in one message");
    if (got == 3) {
        write(b[1], "B", 1);
        char x = 0;
        check(read(fds[1], &x, 1) == 1 && x == 'B', "the second is the second pipe");
        for (int i = 0; i < 3; i++) {
            close(fds[i]);
        }
    }
    // Room for one of three: MSG_CTRUNC, the rest closed.
    send_fds(sv[0], "e", 1, three, 3);
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 1, &got, &flags);
    check(n == 1 && got == 1 && (flags & MSG_CTRUNC), "room for one fd of three: MSG_CTRUNC");
    if (got) {
        close(fds[0]);
    }
    // A plain read drops the fds.
    send_fds(sv[0], "g", 1, three, 1);
    check(read(sv[1], buf, sizeof(buf)) == 1, "a plain read takes the bytes and drops the fds");
    close(a[0]);
    close(b[0]);
    close(c[0]);
    close(a[1]);
    close(b[1]);
    close(c[1]);
    // A bad fd is refused.
    int bad = 999;
    errno = 0;
    check(send_fds(sv[0], "h", 1, &bad, 1) < 0 && errno == EBADF, "passing a bad fd: EBADF");

    // From a child: it passes its own pipe's read end and writes to the pipe.
    pid_t child = fork();
    if (child == 0) {
        int q[2];
        pipe(q);
        send_fds(sv[0], "c", 1, &q[0], 1);
        close(q[0]);
        write(q[1], "from the child", 14);
        close(q[1]);
        _exit(0);
    }
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, NULL);
    int status = -1;
    waitpid(child, &status, 0);
    if (n == 1 && got == 1) {
        memset(buf, 0, sizeof(buf));
        ssize_t r = read(fds[0], buf, sizeof(buf));
        check(r == 14 && memcmp(buf, "from the child", 14) == 0,
              "an fd from a child that has exited still reads what it wrote");
        close(fds[0]);
    } else {
        check(0, "an fd arrives from a child");
    }
    // A socket passed over a socket.
    int inner[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, inner);
    send_fds(sv[0], "s", 1, &inner[1], 1);
    close(inner[1]);
    n = recv_fds(sv[1], buf, sizeof(buf), fds, 8, &got, NULL);
    if (got == 1) {
        send(inner[0], "nested", 6, 0);
        check(recv(fds[0], buf, sizeof(buf), 0) == 6 && memcmp(buf, "nested", 6) == 0, "a socket passed over a socket");
        close(fds[0]);
    } else {
        check(0, "a socket arrives over a socket");
    }
    close(inner[0]);
    close(sv[0]);
    close(sv[1]);
}

// The i915 uAPI pieces this needs (Linux's numbers).
struct gem_create {
    uint64_t size;
    uint32_t handle;
    uint32_t pad;
};
struct prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};
#define IOWR(nr, size) ((3UL << 30) | ((unsigned long)(size) << 16) | ('d' << 8) | (nr))

// A dma-buf of a GPU buffer to a child, which writes its pages; the parent
// reads them through its own mapping.
static void test_dmabuf(void) {
    int drm = open("/dev/dri/renderD128", O_RDWR);
    if (drm < 0) {
        printf("unix-test: skip the dma-buf test: no render node (errno %d)\n", errno);
        return;
    }
    struct gem_create create = {4096, 0, 0};
    int r = ioctl(drm, IOWR(0x40 + 0x1b, sizeof(create)), &create);
    check(r == 0 && create.handle != 0, "a GEM buffer");
    struct prime_handle prime = {create.handle, O_CLOEXEC | O_RDWR, -1};
    r = ioctl(drm, IOWR(0x2d, sizeof(prime)), &prime);
    checkf(r == 0 && prime.fd >= 0, "its dma-buf fd", prime.fd);
    if (r != 0) {
        close(drm);
        return;
    }
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        char buf[8];
        int fds[1], got = 0;
        if (recv_fds(sv[1], buf, sizeof(buf), fds, 1, &got, NULL) != 1 || got != 1) {
            _exit(2);
        }
        uint32_t* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
        if (map == MAP_FAILED) {
            _exit(3);
        }
        for (int i = 0; i < 1024; i++) {
            map[i] = 0xC0DE0000u | (uint32_t)i;
        }
        munmap(map, 4096);
        send(sv[1], "done", 4, 0);
        _exit(0);
    }
    close(sv[1]);
    check(send_fds(sv[0], "b", 1, &prime.fd, 1) == 1, "the dma-buf fd goes to a child");
    char buf[8];
    check(recv(sv[0], buf, sizeof(buf), 0) == 4, "the child wrote its pages");
    int status = -1;
    waitpid(child, &status, 0);
    checkf(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child exits cleanly", WEXITSTATUS(status));
    uint32_t* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, prime.fd, 0);
    int same = map != MAP_FAILED;
    for (int i = 0; same && i < 1024; i++) {
        same = map[i] == (0xC0DE0000u | (uint32_t)i);
    }
    check(same, "the parent's mapping shows what the child wrote");
    if (map != MAP_FAILED) {
        munmap(map, 4096);
    }
    close(sv[0]);
    close(prime.fd);
    close(drm);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    test_stream_pair();
    test_seqpacket();
    test_named();
    test_rights();
    test_dmabuf();
    printf("unix-test: %d checks, %d failed\n", checks, failures);
    printf(failures == 0 ? "unix-test: all checks passed\n" : "unix-test: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
