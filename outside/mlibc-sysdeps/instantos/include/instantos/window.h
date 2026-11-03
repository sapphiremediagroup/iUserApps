/*
 * InstantOS windows for mlibc programs: a compositor window, its events, and
 * surfaces made from GPU buffers. Native programs use ilibcxx's ui::Window;
 * this is the same kernel interface for ported code (Mesa's EGL platform,
 * GL and Vulkan programs). Handles here are kernel handles, not fds; the one
 * fd in it, a dma-buf, goes in through instantos_fd_handle().
 */
#ifndef _INSTANTOS_WINDOW_H
#define _INSTANTOS_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The kernel handle behind an mlibc fd (-1 if none). */
long instantos_fd_handle(int fd);
/* A kernel handle (a dma-buf an InstantOS call gave) as an fd of this
 * process, which then owns it (-1 if the table is full). */
int instantos_handle_fd(long handle);

/* What eglCreateWindowSurface takes as the native window on InstantOS:
 * the window, and the size its surfaces should have (update it when the
 * window is resized; the next frame is drawn at the new size). */
struct instantos_egl_window {
    uint64_t window;
    int32_t width;
    int32_t height;
};

/* Syscall numbers (SyscallNumber in the kernel's syscall.hpp). */
enum {
    INSTANTOS_WSYS_CLOSE = 6,
    INSTANTOS_WSYS_SURFACE_COMMIT = 49,
    INSTANTOS_WSYS_COMPOSITOR_CREATE_WINDOW = 51,
    INSTANTOS_WSYS_WINDOW_EVENT_QUEUE = 52,
    INSTANTOS_WSYS_WINDOW_SET_TITLE = 53,
    INSTANTOS_WSYS_WINDOW_ATTACH_SURFACE = 54,
    INSTANTOS_WSYS_QUEUE_RECEIVE = 62,
    INSTANTOS_WSYS_SERVICE_CONNECT = 66,
    INSTANTOS_WSYS_SURFACE_FROM_BUFFER = 158,
    INSTANTOS_WSYS_SURFACE_WAIT_IDLE = 159,
};

static inline long instantos_syscall(long number, long a1, long a2, long a3, long a4, long a5) {
    register long ret __asm__("rax");
    register long n __asm__("rax") = number;
    register long r1 __asm__("rbx") = a1;
    register long r2 __asm__("r10") = a2;
    register long r3 __asm__("rdx") = a3;
    register long r4 __asm__("r8") = a4;
    register long r5 __asm__("r9") = a5;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Failure is a negative value (older calls give -1). */
static inline int instantos_failed(long value) {
    return value < 0;
}

/* Events (Event in syscall.hpp). */
enum {
    INSTANTOS_EVENT_KEY = 1,
    INSTANTOS_EVENT_POINTER = 2,
    INSTANTOS_EVENT_WINDOW = 3,
};
enum {
    INSTANTOS_KEY_PRESS = 0,
    INSTANTOS_KEY_RELEASE = 1,
    INSTANTOS_KEY_REPEAT = 2,
};
enum {
    INSTANTOS_WINDOW_FOCUS_GAINED = 1,
    INSTANTOS_WINDOW_FOCUS_LOST = 2,
    INSTANTOS_WINDOW_CLOSE_REQUESTED = 3,
    INSTANTOS_WINDOW_RESIZED = 4,
    INSTANTOS_WINDOW_MOVED = 5,
};
/* A few key codes (USB HID usages). */
enum {
    INSTANTOS_KEY_ESCAPE = 0x29,
    INSTANTOS_KEY_SPACE = 0x2C,
    INSTANTOS_KEY_RIGHT = 0x4F,
    INSTANTOS_KEY_LEFT = 0x50,
    INSTANTOS_KEY_DOWN = 0x51,
    INSTANTOS_KEY_UP = 0x52,
};

struct instantos_event {
    uint16_t type;
    uint16_t reserved0;
    uint32_t reserved1;
    union {
        struct {
            uint16_t action;
            uint16_t modifiers;
            uint16_t keycode;
            uint16_t reserved;
            char text[8];
        } key;
        struct {
            uint16_t action;
            uint16_t buttons;
            int32_t x;
            int32_t y;
            int32_t delta_x;
            int32_t delta_y;
        } pointer;
        struct {
            uint16_t action;
            uint16_t reserved0;
            uint32_t window_id;
            int32_t x;
            int32_t y;
            int32_t width;
            int32_t height;
        } window;
        uint8_t raw[48];
    };
};

struct instantos_ipc_message {
    uint64_t id;
    uint32_t sender_pid;
    uint16_t flags;
    uint16_t reserved;
    uint64_t size;
    uint8_t data[256];
};

struct instantos_window {
    uint64_t window;   /* the window handle */
    uint64_t queue;    /* its event queue */
};

/* A window of width x height, titled, focused. 0, or -1. */
static inline int instantos_window_create(struct instantos_window* out, int width, int height, const char* title) {
    long compositor = instantos_syscall(INSTANTOS_WSYS_SERVICE_CONNECT, (long)"graphics.compositor", 0, 0, 0, 0);
    if (instantos_failed(compositor)) {
        return -1;
    }
    long window = instantos_syscall(INSTANTOS_WSYS_COMPOSITOR_CREATE_WINDOW, compositor, width, height, 0, 0);
    instantos_syscall(INSTANTOS_WSYS_CLOSE, compositor, 0, 0, 0, 0);
    if (instantos_failed(window)) {
        return -1;
    }
    long queue = instantos_syscall(INSTANTOS_WSYS_WINDOW_EVENT_QUEUE, window, 0, 0, 0, 0);
    if (instantos_failed(queue)) {
        instantos_syscall(INSTANTOS_WSYS_CLOSE, window, 0, 0, 0, 0);
        return -1;
    }
    if (title) {
        instantos_syscall(INSTANTOS_WSYS_WINDOW_SET_TITLE, window, (long)title, 0, 0, 0);
    }
    out->window = (uint64_t)window;
    out->queue = (uint64_t)queue;
    return 0;
}

static inline void instantos_window_destroy(struct instantos_window* window) {
    instantos_syscall(INSTANTOS_WSYS_CLOSE, (long)window->queue, 0, 0, 0, 0);
    instantos_syscall(INSTANTOS_WSYS_CLOSE, (long)window->window, 0, 0, 0, 0);
}

/* The next event: 1 when one was taken, 0 when none is queued (wait = 0). */
static inline int instantos_window_next_event(const struct instantos_window* window, struct instantos_event* event,
                                              int wait) {
    struct instantos_ipc_message message;
    long r = instantos_syscall(INSTANTOS_WSYS_QUEUE_RECEIVE, (long)window->queue, (long)&message, wait ? 1 : 0, 0, 0);
    if (instantos_failed(r)) {
        return 0;
    }
    __builtin_memcpy(event, message.data, sizeof(*event));
    return 1;
}

/* A surface on a dma-buf fd holding a width x height image, `pitch` bytes a
 * row, BGRA in memory. A handle, or negative. */
static inline long instantos_surface_from_buffer(int dmabuf_fd, uint32_t width, uint32_t height, uint32_t pitch) {
    long handle = instantos_fd_handle(dmabuf_fd);
    if (handle < 0) {
        return -9; /* EBADF */
    }
    return instantos_syscall(INSTANTOS_WSYS_SURFACE_FROM_BUFFER, handle, ((long)width << 32) | height,
                             (long)pitch << 32, 0, 0);
}

/* Shows the surface in the window and commits it (once the GPU has finished
 * drawing it). */
static inline void instantos_window_present(uint64_t window, long surface, uint32_t width, uint32_t height) {
    /* Commit first: it waits for the GPU and hands the compositor the
     * surface, so a window never shows a surface it has no pixels for. */
    instantos_syscall(INSTANTOS_WSYS_SURFACE_COMMIT, surface, 0, 0, ((long)width << 32) | height, 0);
    instantos_syscall(INSTANTOS_WSYS_WINDOW_ATTACH_SURFACE, (long)window, surface, 0, 0, 0);
}

/* 0 once no window shows the surface and the compositor let it go; negative
 * on timeout. */
static inline long instantos_surface_wait_idle(long surface, uint64_t timeout_ms) {
    return instantos_syscall(INSTANTOS_WSYS_SURFACE_WAIT_IDLE, surface, (long)timeout_ms, 0, 0, 0);
}

static inline void instantos_handle_close(long handle) {
    instantos_syscall(INSTANTOS_WSYS_CLOSE, handle, 0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif
