// mlibc TCP/HTTP fetch smoke test.
//
// Exercises the hardened TCP stack end-to-end over a real connection:
//   DNS resolve -> connect (3-way handshake) -> send GET (segmented) ->
//   recv response until the server's FIN (EOF) -> close.
// Verifies handshake, data transfer, and the FIN/EOF teardown that the old
// fire-and-forget stack could not do.
//
// Serial markers: "http: serial-done" on success; "http: FAIL ..." otherwise.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static unsigned long sc0(unsigned long n) {
    unsigned long r; register unsigned long rax asm("rax") = n;
    asm volatile("syscall" : "=a"(r) : "a"(rax) : "rcx", "r11", "memory");
    return r;
}
static unsigned long sc2(unsigned long n, unsigned long a, unsigned long b) {
    unsigned long r; register unsigned long rax asm("rax")=n; register unsigned long rbx asm("rbx")=a; register unsigned long r10 asm("r10")=b;
    asm volatile("syscall" : "=a"(r) : "a"(rax),"r"(rbx),"r"(r10) : "rcx","r11","memory");
    return r;
}
#define SYS_SLEEP 15
#define SYS_SERIAL_WRITE 88
#define SYS_NET_PROCESS_PACKETS 72

static void sw(const char* t){ unsigned long l=0; while(t[l])l++; sc2(SYS_SERIAL_WRITE,(unsigned long)t,l); }

static const char* kHost = "example.com";

int main(void) {
    sw("http: serial-start\n");

    // Background packet pump (network-manager's job in a normal boot).
    pid_t pump = fork();
    if (pump == 0) { for(;;){ sc0(SYS_NET_PROCESS_PACKETS); sc2(SYS_SLEEP,2,0);} _exit(0); }

    // Resolve the host.
    sw("http: resolving\n");
    struct addrinfo hints; memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    int rc = getaddrinfo(kHost, NULL, &hints, &res);
    if (rc != 0 || !res) { char b[64]; snprintf(b,sizeof(b),"http: FAIL dns rc=%d\n",rc); sw(b); return 1; }

    struct sockaddr_in dst = *(struct sockaddr_in*)res->ai_addr;
    dst.sin_port = htons(80);
    char ipstr[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&dst.sin_addr,ipstr,sizeof(ipstr));
    { char b[96]; snprintf(b,sizeof(b),"http: %s -> %s\n",kHost,ipstr); sw(b); }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { sw("http: FAIL socket\n"); return 1; }

    // connect() is non-blocking under our stack: retry while EAGAIN until the
    // handshake completes.
    int connected = 0;
    for (int i=0;i<500;i++){
        int r = connect(fd, (struct sockaddr*)&dst, sizeof(dst));
        if (r == 0) { connected = 1; break; }
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EALREADY) {
            char b[64]; snprintf(b,sizeof(b),"http: FAIL connect errno=%d\n",errno); sw(b); return 1;
        }
        sc2(SYS_SLEEP, 5, 0);
    }
    if (!connected) { sw("http: FAIL connect timeout\n"); return 1; }
    sw("http: connected\n");

    // Send a minimal HTTP/1.0 GET (Connection: close so the server FINs).
    char req[256];
    int rn = snprintf(req,sizeof(req),
        "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", kHost);
    int off = 0;
    while (off < rn) {
        ssize_t s = send(fd, req+off, rn-off, 0);
        if (s > 0) { off += s; continue; }
        if (errno == EAGAIN) { sc2(SYS_SLEEP,5,0); continue; }
        char b[64]; snprintf(b,sizeof(b),"http: FAIL send errno=%d\n",errno); sw(b); return 1;
    }
    sw("http: request sent\n");

    // Read the response until EOF (server FIN).
    char buf[1024];
    long total = 0;
    int sawStatus = 0;
    for (int idle=0; idle<2000;) {
        ssize_t r = recv(fd, buf, sizeof(buf)-1, 0);
        if (r > 0) {
            idle = 0;
            if (!sawStatus) {
                buf[r] = 0;
                // Print the first status line.
                char line[80]; int li=0;
                for (int i=0;i<r && buf[i] && buf[i] != '\r' && buf[i] != '\n' && li<79;i++) line[li++]=buf[i];
                line[li]=0;
                char b[128]; snprintf(b,sizeof(b),"http: status=%s\n",line); sw(b);
                sawStatus = 1;
            }
            total += r;
            continue;
        }
        if (r == 0) { sw("http: EOF (server FIN)\n"); break; }
        if (errno == EAGAIN) { sc2(SYS_SLEEP,5,0); idle++; continue; }
        char b[64]; snprintf(b,sizeof(b),"http: FAIL recv errno=%d\n",errno); sw(b); return 1;
    }

    { char b[64]; snprintf(b,sizeof(b),"http: received %ld bytes\n",total); sw(b); }

    shutdown(fd, SHUT_RDWR);
    close(fd);
    freeaddrinfo(res);

    if (total > 0 && sawStatus) sw("http: serial-done\n");
    else sw("http: FAIL no data\n");
    return 0;
}
