// netsurf: starts the NetSurf browser (/bin/nsfb) in a compositor window
// sized to the screen, with its resources and a writable home. Run by the
// desktop's WEB button; arguments (a URL) are passed on.
//
//   netsurf [url]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYS_FB_INFO 18
#define NSFB_PATH "/bin/nsfb"

// FBInfo: the screen geometry; addr stays 0 for anyone but the compositor.
struct fb_info {
    uint64_t addr;
    uint32_t width, height, pitch, format;
    uint64_t size;
    uint32_t red_mask, green_mask, blue_mask;
};

static long sys_fb_info(struct fb_info* info) {
    long r;
    register long rbx __asm__("rbx") = (long)info;
    __asm__ volatile("syscall" : "=a"(r) : "a"((long)SYS_FB_INFO), "r"(rbx) : "rcx", "r11", "memory");
    return r;
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int main(int argc, char** argv) {
    // Leave room for the window frame and the taskbar.
    int width = 1024, height = 700;
    struct fb_info fb;
    memset(&fb, 0, sizeof(fb));
    if (sys_fb_info(&fb) == 0 && fb.width && fb.height) {
        width = clamp((int)fb.width - 120, 640, 1280);
        height = clamp((int)fb.height - 170, 400, 900);
    }
    char w[16], h[16];
    snprintf(w, sizeof(w), "%d", width);
    snprintf(h, sizeof(h), "%d", height);

    // NetSurf keeps its choices and cookies under $HOME/.netsurf.
    setenv("HOME", getenv("HOME") ? getenv("HOME") : "/tmp", 0);
    setenv("NETSURFRES", "/netsurf/res/", 0);

    char* args[16];
    int n = 0;
    args[n++] = "nsfb";
    args[n++] = "-f";
    args[n++] = "instant";
    args[n++] = "-b";
    args[n++] = "32";
    args[n++] = "-w";
    args[n++] = w;
    args[n++] = "-h";
    args[n++] = h;
    for (int i = 1; i < argc && n < 15; i++) {
        args[n++] = argv[i];
    }
    args[n] = NULL;
    execv(NSFB_PATH, args);
    perror("netsurf: " NSFB_PATH);
    return 127;
}
