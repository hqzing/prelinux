# glibc patch series

`series` is the apply order, `patches/` holds every patch, in three layers:

| Numbers | Layer | Contents |
|---|---|---|
| 0001–0006 | HongMeng kernel compatibility | rseq off by default, never probe clone3, `posix_spawn` uses clone, `epoll_pwait2` translated to `epoll_pwait`, syscall block list, ld.so maps executable pages per segment |
| 0007–0021 | Path translation and privilege softening | placeholder and translation core, the open/stat/access/directory/link/attribute/exec entry points, syscalls.list adjustments, fake ids, uname, creat |
| 0022–0027 | Package manager compatibility | fchmodat2, getxattr, close_range, listxattr, fchmodat with an empty path, removexattr |

The kernel compatibility layer comes first: on the HongMeng kernel a program that does not carry these patches is killed by SIGSYS at startup or at the first `posix_spawn`.

Those patches come from probing the kernel item by item on a HarmonyOS PC. The number table in `0005-syscall-filter` is that probe's result. They rearrange the call paths of upstream glibc without changing semantics, and are no-ops on a Linux kernel, so one build serves both.

The series is based on glibc 2.39.
