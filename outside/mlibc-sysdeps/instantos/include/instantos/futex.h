/*
 * InstantOS futexes, for ported code that waits on a word itself (Mesa, libc++);
 * everything else uses pthreads, which sleep on them. Words are 32 bits, 4-byte
 * aligned, and belong to the address space, except on memory shared between
 * processes (a dma-buf, InstantOS shared memory), where either side wakes the
 * other.
 */
#ifndef _INSTANTOS_FUTEX_H
#define _INSTANTOS_FUTEX_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sleeps until woken, if the word still holds `expected`. `timeout` is relative
 * (NULL: none). 0 once woken, else an errno value: EAGAIN (another value),
 * ETIMEDOUT, EINTR, EINVAL, EFAULT. Spurious returns of 0 are allowed. */
int instantos_futex_wait(const volatile uint32_t *address, uint32_t expected, const struct timespec *timeout);

/* Wakes up to `count` waiters, the longest waiting first; returns how many. */
int instantos_futex_wake(const volatile uint32_t *address, int count);

#ifdef __cplusplus
}
#endif

#endif
