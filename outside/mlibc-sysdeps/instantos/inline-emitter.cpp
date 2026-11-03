// Out-of-line copies of the __MLIBC_INLINE_DEFINITION functions in the headers
// this port installs itself (mlibc's own emitter only covers sys/sysmacros.h
// when the linux option is on, which it is not here). Without them a call the
// compiler does not inline (major()/minor()/makedev() at -O0, or GNU tar's
// gnulib) fails to link against libc.so.

#define __MLIBC_EMIT_INLINE_DEFINITIONS

#include <sys/sysmacros.h>
