/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux path translation — core definitions.
   This header implements the whole mapping logic of PreLinux (see
   docs/design.md chapter 5).  It is compiled into glibc (both libc and
   the dynamic loader) and into the base SDK tools, so it must obey
   strict constraints:

   - async-signal-safe: callable from signal handler context;
   - no heap allocation, caller provides the buffer;
   - no locks, no mutable global state;
   - prefix matching over static tables only, O(table length);
   - no libc function calls in the translate path (usable during early
     rtld bootstrap);
   - matching is path-segment-boundary aware: "/usr" matches
     "/usr/bin/x" but not "/usrshare/x".

   The instance root is a compile-time placeholder string of fixed
   capacity (512 bytes) that `prelinux relocate` rewrites in place at
   install time (equal or shorter, NUL padded).  ld.so reads it as a
   plain constant: there is no runtime self-location logic anywhere.

   The block between "BEGIN SHARED CORE" and "END SHARED CORE" is
   byte-identical with the glibc-side header (added by
   glibc/patches/0001-prelinux-core.patch as include/prelinux.h);
   base/tests/test-sync.sh enforces this.  Do not edit one copy without
   the other.  */

#ifndef _PRELINUX_TRANSLATE_H
#define _PRELINUX_TRANSLATE_H 1

#include <stddef.h>
#include <limits.h>
#include <errno.h>

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif
#ifndef __set_errno
# define __set_errno(val) (errno = (val))
#endif
#ifndef __glibc_likely
# define __glibc_likely(cond) __builtin_expect ((cond), 1)
#endif
#ifndef __glibc_unlikely
# define __glibc_unlikely(cond) __builtin_expect ((cond), 0)
#endif

/* ===== BEGIN SHARED CORE ============================================ */

/* Capacity of the instance-root slot, including the terminating NUL.
   The instance root path must therefore be at most 254 bytes long.

   The cap is 255, not the 512 of early drafts: the placeholder itself
   is a single path component that `make install` must be able to
   mkdir, and NAME_MAX is 255 on relevant filesystems.  (The kernel's
   255-byte shebang limit is the tighter practical constraint anyway,
   enforced by `prelinux import`.)  */
#define PRELINUX_ROOT_CAPACITY 255

/* The storage is defined exactly once per binary module
   (prelinux-root.c in glibc, translate.c in the SDK).  It is extern so
   the compiler cannot fold the contents into code — the bytes are
   rewritten at install time.  Hidden visibility keeps references
   PC-relative so ld.so can read it before self-relocation.  */
extern const char __prelinux_root_storage[PRELINUX_ROOT_CAPACITY]
  __attribute__ ((visibility ("hidden")));

/* The pristine placeholder value: 254 bytes + NUL.  Every occurrence of
   this byte pattern in the built binaries is rewritten by
   `prelinux relocate` with the real instance root (NUL padded to the
   full slot).  Kept free of any runtime detection logic on purpose.  */
#define PRELINUX_ROOT_PLACEHOLDER                                          \
  "/.prelinux-root-placeholder-"                                           \
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 50  */           \
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 100 */           \
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 150 */           \
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 200 */           \
  "AAAAAAAAAAAAAAAAAAAAAAAAAA"                             /* 226 */

/* Mapping prefix table and pass-through table, as NUL-separated
   lists with a terminating empty entry.  Builds
   may override either with -DPRELINUX_MAP_TABLE="..." /
   -DPRELINUX_PASSTHROUGH_TABLE="...".  */
#ifndef PRELINUX_MAP_TABLE
# define PRELINUX_MAP_TABLE                                                 \
  "/bin\0/sbin\0/lib\0/lib64\0/usr\0/etc\0/var\0/opt\0/srv\0/home\0"       \
  "/root\0/boot\0/run\0/tmp\0"
#endif
#ifndef PRELINUX_PASSTHROUGH_TABLE
# define PRELINUX_PASSTHROUGH_TABLE                                         \
  "/proc\0/sys\0/dev\0/selinux\0/storage\0/mnt\0/media\0/data\0/system\0/vendor\0"
#endif

static const char __prelinux_map_table[] = PRELINUX_MAP_TABLE;
static const char __prelinux_passthrough_table[] = PRELINUX_PASSTHROUGH_TABLE;

/* Tiny string helpers: no libc calls allowed here (early rtld, signal
   handler context).  */
static inline size_t
__prelinux_strlen (const char *s)
{
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  return n;
}

/* Return nonzero if PATH equals PREFIX or starts with PREFIX + '/'.
   PLEN must be __prelinux_strlen (PREFIX).  */
static inline int
__prelinux_prefix_match (const char *path, const char *prefix, size_t plen)
{
  size_t i;
  for (i = 0; i < plen; i++)
    if (path[i] != prefix[i])
      return 0;
  return path[i] == '\0' || path[i] == '/';
}

/* Return nonzero if PATH matches any entry of the NUL-separated table.  */
static inline int
__prelinux_table_match (const char *path, const char *table)
{
  while (*table != '\0')
    {
      size_t plen = __prelinux_strlen (table);
      if (__prelinux_prefix_match (path, table, plen))
        return 1;
      table += plen + 1;
    }
  return 0;
}

/* The single implementation of the mapping rules.

   Returns PATH unchanged when no rule applies; returns BUF with the
   translated path when a mapping prefix matches; returns NULL when the
   result would not fit into BUFLEN bytes.

   Rule order (idempotency before mapping, so that an instance root
   below a mapped prefix such as /root keeps working):
   0. below the placeholder    -> real root + remainder
   1. exactly "/"              -> the root itself (dirfd walks anchored
                                  at "/" must not escape)
   2. relative path            -> unchanged
   3. already under root       -> unchanged
   4. pass-through prefix      -> unchanged
   5. mapping prefix           -> root + path
   6. any other absolute path  -> unchanged (compat-matrix signal)

   Rule 0 covers the glibc build's prefix-derived strings (libdir,
   gconv and locale dirs, RUNTIME_LINKER): they are compiled in as
   PLACEHOLDER + "/glibc/..." and are never rewritten on disk
   (the kernel-consumed PT_INTERP is the exception; relocate rewrites
   it physically).  */
static inline const char *
__prelinux_translate_path_root (const char *root, const char *path,
				char *buf, size_t buflen)
{
  if (__glibc_unlikely (path == NULL))
    return NULL;
  if (path[0] != '/')
    return path;

  size_t rlen = __prelinux_strlen (root);
  size_t plen = __prelinux_strlen (path);
  char *p;
  size_t i;

  /* Rule 0: substitute the real root for the placeholder prefix.  The
     placeholder is always exactly PRELINUX_ROOT_CAPACITY-1 bytes, so
     the remainder begins at a fixed offset.  The 28-byte fixed stem is
     matched explicitly; the padding region is trusted to be the
     placeholder's (no real path looks like this).  */
  if (plen >= PRELINUX_ROOT_CAPACITY - 1)
    {
      const char *ph = "/.prelinux-root-placeholder-";
      for (i = 0; ph[i] != '\0'; i++)
	if (path[i] != ph[i])
	  break;
      if (ph[i] == '\0'
	  && (path[PRELINUX_ROOT_CAPACITY - 1] == '\0'
	      || path[PRELINUX_ROOT_CAPACITY - 1] == '/'))
	{
	  const char *suffix = path + (PRELINUX_ROOT_CAPACITY - 1);
	  size_t slen = plen - (PRELINUX_ROOT_CAPACITY - 1);
	  if (__glibc_unlikely (rlen + slen + 1 > buflen))
	    return NULL;
	  p = buf;
	  for (i = 0; i < rlen; i++)
	    *p++ = root[i];
	  for (i = 0; i <= slen; i++)
	    *p++ = suffix[i];
	  return buf;
	}
    }

  /* Rule 1: the bare "/" maps to the instance root itself.  A process
     that opens "/" and then descends with dirfd-relative *at() calls
     (rpm's fsm payload writer) presents no absolute path for the
     interception points to rewrite; anchoring "/" itself keeps those
     walks inside the instance.  */
  if (plen == 1)
    return rlen == 1 ? path : root;

  /* Root is "/" or PATH is already below the root: idempotent.  */
  if (rlen == 1 || __prelinux_prefix_match (path, root, rlen))
    return path;

  if (__prelinux_table_match (path, __prelinux_passthrough_table))
    return path;

  if (!__prelinux_table_match (path, __prelinux_map_table))
    return path;

  if (__glibc_unlikely (rlen + plen + 1 > buflen))
    return NULL;

  p = buf;
  for (i = 0; i < rlen; i++)
    *p++ = root[i];
  for (i = 0; i <= plen; i++)
    *p++ = path[i];
  return buf;
}

/* The normal entry point: translate against this module's compiled-in
   instance root (the install-time-rewritten constant).  Tools that must
   translate against a DIFFERENT root (e.g. relocate operating on a
   tree) call __prelinux_translate_path_root directly.  */
static inline const char *
__prelinux_translate_path (const char *path, char *buf, size_t buflen)
{
  return __prelinux_translate_path_root (__prelinux_root_storage, path,
					 buf, buflen);
}

/* ===== END SHARED CORE ============================================== */

/* Call-site wrapper used by the patched glibc entry points: translates
   PATH (a const char * lvalue) in place, or fails with ENAMETOOLONG.
   Only usable in functions that return -1 on error.

   The scratch buffer is declared at function scope and its name derives
   from the argument: functions translating two paths (link, rename...)
   need both translations alive at once, and a block-scoped buffer could
   share its stack slot with a sibling block (observed misbehavior).  */
#define PRELINUX_TRANSLATE(path)                                           \
  char __prelinux_buf_##path[PATH_MAX];                                    \
  do                                                                       \
    {                                                                      \
      if ((path) != NULL && (path)[0] == '/')                              \
        {                                                                  \
          const char *__prelinux_res                                       \
            = __prelinux_translate_path ((path), __prelinux_buf_##path,    \
                                         sizeof (__prelinux_buf_##path));  \
          if (__glibc_unlikely (__prelinux_res == NULL))                   \
            {                                                              \
              __set_errno (ENAMETOOLONG);                                  \
              return -1;                                                   \
            }                                                              \
          (path) = __prelinux_res;                                         \
        }                                                                  \
    }                                                                      \
  while (0)

/* Privilege softening: chown/chmod/setxattr-style calls
   that fail with EPERM for a non-root euid pretend success — ownership
   metadata is meaningless inside an instance, and this lets dpkg/rpm
   run as an ordinary user.  RET must be the raw return value of the
   wrapped call, with errno still live.  */
static inline int
__prelinux_soften_priv (int ret, long euid)
{
  if (ret == -1 && errno == EPERM && euid != 0)
    return 0;
  return ret;
}

/* The softening helper needs a geteuid primitive; only compile it where
   one is available (glibc syscall-wrapper files all include <sysdep.h>,
   hence INLINE_SYSCALL_CALL; the standalone SDK build uses syscall()).
   Translation-only TUs (prelinux-root.c, rtld files) include this
   header without either and simply skip the helper.  */
#if defined (PRELINUX_STANDALONE)
# include <unistd.h>
# include <sys/syscall.h>
static inline long
__prelinux_geteuid (void)
{
  return syscall (SYS_geteuid);
}
# define PRELINUX_SOFTEN_PRIV_AVAILABLE 1
#elif defined (INLINE_SYSCALL_CALL)
static inline long
__prelinux_geteuid (void)
{
  return INLINE_SYSCALL_CALL (geteuid);
}
# define PRELINUX_SOFTEN_PRIV_AVAILABLE 1
#endif

#ifdef PRELINUX_SOFTEN_PRIV_AVAILABLE
#define PRELINUX_SOFTEN_PRIV(ret)                                          \
  __prelinux_soften_priv ((ret), __prelinux_geteuid ())
#endif

#endif /* _PRELINUX_TRANSLATE_H */
