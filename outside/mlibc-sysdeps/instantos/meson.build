sysdep_supported_options = {
  'posix': true,
}

rtld_sources += files(
  'syscall.cpp',
  'sysdeps.cpp',
)
rtld_include_dirs += include_directories('include')

libc_sources += files(
  'builtins.c',
  'entry.cpp',
  'syscall.cpp',
  'sysdeps.cpp',
)
libc_include_dirs += include_directories('include')

if not no_headers
  install_headers(
    '../../abis/linux/access.h',
    '../../abis/linux/auxv.h',
    '../../abis/linux/blkcnt_t.h',
    '../../abis/linux/blksize_t.h',
    '../../abis/linux/clockid_t.h',
    '../../abis/linux/dev_t.h',
    '../../abis/linux/errno.h',
    '../../abis/linux/fcntl.h',
    '../../abis/linux/fd_set.h',
    '../../abis/linux/fsblkcnt_t.h',
    '../../abis/linux/fsfilcnt_t.h',
    '../../abis/linux/gid_t.h',
    '../../abis/linux/in.h',
    '../../abis/linux/ino_t.h',
    '../../abis/linux/limits.h',
    '../../abis/linux/mode_t.h',
    '../../abis/linux/nlink_t.h',
    '../../abis/linux/pid_t.h',
    '../../abis/linux/poll.h',
    '../../abis/linux/resource.h',
    '../../abis/linux/rlim_t.h',
    '../../abis/linux/sa_family_t.h',
    '../../abis/linux/seek-whence.h',
    '../../abis/linux/sem.h',
    '../../abis/linux/sig-limits.h',
    '../../abis/linux/sigevent.h',
    '../../abis/linux/signal.h',
    '../../abis/linux/sigset_t.h',
    '../../abis/linux/sigval.h',
    '../../abis/linux/sockaddr_storage.h',
    '../../abis/linux/socket.h',
    '../../abis/linux/socklen_t.h',
    '../../abis/linux/stat.h',
    '../../abis/linux/statvfs.h',
    '../../abis/linux/suseconds_t.h',
    '../../abis/linux/termios.h',
    '../../abis/linux/time.h',
    '../../abis/linux/uid_t.h',
    '../../abis/linux/utsname.h',
    '../../abis/linux/vm-flags.h',
    '../../abis/linux/wait.h',
    subdir: 'abi-bits',
    follow_symlinks: true,
  )
endif

if not headers_only
  custom_target('crt1',
    build_by_default: true,
    command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
    input: 'crt1.S',
    output: 'crt1.o',
    install: true,
    install_dir: get_option('libdir'),
  )
endif
