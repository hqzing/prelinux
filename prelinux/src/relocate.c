/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux base SDK — `prelinux relocate`.
   The base's only file-rewriting tool.  For every argument (files or
   directories, the latter walked recursively):

   - ELF executables/shared libraries: PT_INTERP is set to the
     instance's ld.so; absolute RPATH/RUNPATH entries are translated;
   - text files with an absolute-path shebang: the interpreter is
     rewritten to the instance path (except #!/usr/bin/env forms);
   - every file is scanned for the 255-byte instance-root slot(s) and
     rewrites them (placeholder on first install, previous root when the
     instance was moved);
   - every ELF is (re)signed with the vendored ohos-selfsign code after
     any modification — inert on hosts that don't enforce signatures.

   Unwritable targets (verified-signed files on HarmonyOS, or
   text-busy files on Linux) fall back to read-recreate-replace.

   Path math never hand-rolls mapping rules: it goes through the shared
   translation core (prelinux-translate.h).  */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRELINUX_STANDALONE 1
#include "prelinux-translate.h"
#include "prelinux.h"

static const char *g_root;      /* instance root being written to */
static const char *g_oldroot;   /* previous root (moved instances) */
static int g_quiet;
static int g_nosign;
static unsigned long g_n_interp, g_n_rpath, g_n_shebang, g_n_slots,
  g_n_links, g_n_signed, g_n_skip, g_n_err;

/* ld.so file name per ELF machine type.  */
static const char *
ldso_name (unsigned machine)
{
  switch (machine)
    {
    case 183: /* EM_AARCH64 */
      return "ld-linux-aarch64.so.1";
    case 62: /* EM_X86_64 */
      return "ld-linux-x86-64.so.2";
    default:
      return NULL;
    }
}

/* Hook for elf.c: translate one RPATH element; NULL keeps it.  */
char *
prelinux_relocate_rpath_element (const char *elem)
{
  if (elem[0] != '/' || strncmp (elem, "$ORIGIN", 7) == 0)
    return NULL;
  char buf[PATH_MAX];
  char rootbuf[PATH_MAX];
  /* Already under the target root: keep.  */
  if (__prelinux_prefix_match (elem, g_root, __prelinux_strlen (g_root)))
    return NULL;
  /* An element under the OLD root (moved instance): re-anchor.  */
  if (g_oldroot != NULL
      && __prelinux_prefix_match (elem, g_oldroot,
				  __prelinux_strlen (g_oldroot)))
    {
      size_t ol = __prelinux_strlen (g_oldroot);
      snprintf (rootbuf, sizeof (rootbuf), "%s%s", g_root, elem + ol);
      return strdup (rootbuf);
    }
  const char *r = __prelinux_translate_path_root (g_root, elem, buf,
						  sizeof (buf));
  if (r == NULL || r == elem)
    return NULL;
  (void) rootbuf;
  return strdup (r);
}

static int
relocate_elf (const char *path)
{
  char err[512];
  err[0] = '\0';
  prelinux_elf *e = prelinux_elf_open (path, err, sizeof (err));
  if (e == NULL)
    {
      if (err[0])
	{
	  fprintf (stderr, "relocate: %s\n", err);
	  g_n_err++;
	  return -1;
	}
      g_n_skip++; /* not a relocatable target (e.g. ET_REL) */
      return 0;
    }

  int ret = 0;
  if (prelinux_elf_has_interp (e))
    {
      /* Heal binaries imported before the ET_EXEC load-bias fix: their
	 appended interpreter page sits at vaddr == file offset, which
	 the kernel turns into a bogus AT_PHDR (SIGSEGV on every exec of
	 a non-PIE binary).  Shifting it back is in-place and idempotent.  */
      int fixed = prelinux_elf_fix_exec_interp_page (e);
      if (fixed < 0)
	{
	  fprintf (stderr, "relocate: %s: cannot repair interpreter page\n",
		   path);
	  g_n_err++;
	  prelinux_elf_close (e);
	  return -1;
	}
      if (fixed > 0)
	{
	  g_n_interp++;
	  if (!g_quiet)
	    printf ("%s: ET_EXEC interpreter page moved to load_bias+offset\n",
		    path);
	}
      const char *ld = ldso_name ((unsigned) prelinux_elf_machine (e));
      if (ld == NULL)
	{
	  fprintf (stderr, "relocate: %s: unsupported ELF machine\n", path);
	  g_n_err++;
	  prelinux_elf_close (e);
	  return -1;
	}
      char want[PATH_MAX];
      snprintf (want, sizeof (want), "%s/glibc/lib/%s", g_root, ld);
      /* The kernel resolves PT_INTERP itself through a
	 BINPRM_BUF_SIZE (256-byte) buffer: HarmonyOS PC fails execve
	 with ENOMEM once the path is 255 bytes or longer, and Linux
	 would silently truncate.  Refuse with a clear message instead
	 of writing binaries that cannot run (the import
	 root-length limit is derived from the same constant).
	 Exception: the pristine placeholder root used at packaging time
	 is 254 bytes by design, so a placeholder-rooted interpreter is
	 necessarily longer; those files are never executed before import
	 rewrites them to the real (bounded) instance root.  */
      if (!prelinux_root_is_placeholder (g_root)
	  && strlen (want) > PRELINUX_INTERP_MAX)
	{
	  fprintf (stderr,
		   "relocate: %s: instance root too deep: interpreter "
		   "path is %zu bytes, the kernel limit is %d; choose a "
		   "shallower instance root\n",
		   path, strlen (want), PRELINUX_INTERP_MAX);
	  g_n_err++;
	  prelinux_elf_close (e);
	  return -1;
	}
      const char *cur = prelinux_elf_get_interp (e);
      /* Rewrite when the path differs, or when the segment is larger
	 than the string it holds: import shrinks segments that an
	 earlier placeholder-root pass left oversized (see elf.c).  */
      int size_oversized =
	prelinux_elf_interp_size (e) > strlen (want) + 1;
      if (cur == NULL || strcmp (cur, want) != 0 || size_oversized)
	{
	  if (prelinux_elf_set_interp (e, want, err, sizeof (err)) != 0)
	    {
	      fprintf (stderr, "relocate: %s: set-interpreter: %s\n", path,
		       err);
	      g_n_err++;
	      ret = -1;
	    }
	  else
	    {
	      g_n_interp++;
	      if (!g_quiet)
		printf ("%s: interpreter -> %s\n", path, want);
	    }
	}
    }

  int rr = prelinux_elf_rewrite_rpath (e, err, sizeof (err));
  if (rr < 0)
    {
      fprintf (stderr, "relocate: %s: rpath: %s\n", path, err);
      g_n_err++;
      ret = -1;
    }
  else if (rr > 0)
    {
      g_n_rpath++;
      if (!g_quiet)
	printf ("%s: rpath rewritten\n", path);
    }

  prelinux_elf_close (e);
  return ret;
}

/* Rewrite an absolute-path shebang at the start of PATH's file.  */
static int
relocate_shebang (const char *path, int fd)
{
  /* Shebangs are rewritten only against real instance roots: with the
     254-byte placeholder (packaging time) nothing would fit the
     kernel's 255-byte shebang limit; import rewrites them later.  */
  if (prelinux_root_is_placeholder (g_root))
    return 0;
  struct stat st;
  if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
    return 0;
  off_t fsize = st.st_size;
  if (fsize < 4)
    return 0;

  char head[1024];
  ssize_t n = pread (fd, head, sizeof (head) - 1, 0);
  if (n < 4 || head[0] != '#' || head[1] != '!')
    return 0;
  head[n] = '\0';
  char *eol = memchr (head, '\n', n);
  if (eol == NULL)
    return 0; /* single-line script: nothing to preserve after it */
  *eol = '\0';

  /* Parse "#! [spaces] interp [args]" — kernel BINPRM rules.  */
  char *p = head + 2;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '/')
    return 0; /* relative or empty interpreter: leave alone */
  char *args = strchr (p, ' ');
  char *interp;
  if (args)
    {
      *args = '\0';
      interp = p;
      args++;
    }
  else
    interp = p;

  char newinterp[PATH_MAX];
  size_t ilen;
  if (__prelinux_prefix_match (interp, g_root, __prelinux_strlen (g_root)))
    return 0; /* already instance-local */
  if (g_oldroot != NULL
      && __prelinux_prefix_match (interp, g_oldroot,
				  __prelinux_strlen (g_oldroot)))
    {
      size_t ol = __prelinux_strlen (g_oldroot);
      if (strlen (g_root) + strlen (interp) - ol + 1 > sizeof (newinterp))
	goto toolong;
      sprintf (newinterp, "%s%s", g_root, interp + ol);
      goto rewrite;
    }
  if (strcmp (interp, "/usr/bin/env") == 0)
    return 0; /* env form is left alone */
  {
    char buf[PATH_MAX];
    const char *r = __prelinux_translate_path_root (g_root, interp, buf,
						    sizeof (buf));
    if (r == NULL || r == interp)
      return 0; /* unmapped or pass-through: leave alone */
    snprintf (newinterp, sizeof (newinterp), "%s", r);
  }

 rewrite:
  ilen = strlen (newinterp);
  {
    size_t newline = 2 + ilen + (args ? 1 + strlen (args) : 0);
    if (newline > 255)
      goto toolong;
  }

  /* Rewrite the file with the new first line (in place, same inode).  */
  {
    size_t rest = (size_t) fsize - ((eol - head) + 1);
    char *content = malloc (rest);
    if (rest && pread (fd, content, rest, (eol - head) + 1) != (ssize_t) rest)
      {
	free (content);
	return 0;
      }
    if (ftruncate (fd, 0) != 0)
      {
	free (content);
	return 0;
      }
    /* relocate_one() read the magic bytes from this fd: its file offset
       is 4 now, and write() uses the current offset.  Rewind.  */
    lseek (fd, 0, SEEK_SET);
    char linebuf[1024];
    int ll = snprintf (linebuf, sizeof (linebuf), "#!%s%s%s\n", newinterp,
		       args ? " " : "", args ? args : "");
    ssize_t w1 = write (fd, linebuf, (size_t) ll);
    ssize_t w2 = rest ? write (fd, content, rest) : 0;
    free (content);
    if (w1 != ll || w2 != (ssize_t) rest)
      {
	fprintf (stderr, "relocate: %s: shebang rewrite failed\n", path);
	g_n_err++;
	return -1;
      }
  }
  g_n_shebang++;
  if (!g_quiet)
    printf ("%s: shebang -> %s\n", path, newinterp);
  return 0;

 toolong:
  fprintf (stderr,
	   "relocate: %s: translated shebang exceeds the kernel 255-byte "
	   "limit; choose a shallower instance root\n", path);
  g_n_err++;
  return -1;
}

/* Copy content + mode to a sibling temp path (for the delete+recreate
   fallback).  */
static int
copy_to_temp (const char *path, char *tmp, size_t tmplen)
{
  int in = open (path, O_RDONLY);
  if (in < 0)
    return -1;
  struct stat st;
  if (fstat (in, &st) != 0)
    {
      close (in);
      return -1;
    }
  snprintf (tmp, tmplen, "%s.prelinux-new", path);
  int out = open (tmp, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
  if (out < 0)
    {
      close (in);
      return -1;
    }
  char buf[65536];
  ssize_t n;
  int ret = 0;
  while ((n = read (in, buf, sizeof (buf))) > 0)
    if (write (out, buf, (size_t) n) != n)
      ret = -1;
  close (in);
  if (close (out) != 0)
    ret = -1;
  return ret;
}

/* EM_AARCH64 (183) from the ELF header of an open file.  */
static int
elf_is_aarch64 (int fd)
{
  unsigned char m[2];
  return pread (fd, m, 2, 18) == 2 && m[0] == 183 && m[1] == 0;
}

/* Per-file work, after path normalization.  */
static int
relocate_one_real (const char *path, int depth)
{
  int fd = open (path, O_RDWR);
  if (fd < 0)
    {
      /* Fallback for unwritable files: verified-signed binaries on
	 HarmonyOS (EACCES), running binaries on Linux (ETXTBSY).  */
      if ((errno == EACCES || errno == EPERM || errno == ETXTBSY)
	  && depth == 0)
	{
	  char tmp[PATH_MAX];
	  if (copy_to_temp (path, tmp, sizeof (tmp)) == 0)
	    {
	      int r = relocate_one_real (tmp, depth + 1);
	      if (r == 0)
		{
		  unlink (path);
		  if (rename (tmp, path) != 0)
		    {
		      fprintf (stderr, "relocate: %s: replace failed: %s\n",
			       path, strerror (errno));
		      unlink (tmp);
		      g_n_err++;
		      return -1;
		    }
		}
	      else
		unlink (tmp);
	      return r;
	    }
	}
      g_n_skip++;
      return 0;
    }
  unsigned char magic[4];
  ssize_t n = read (fd, magic, 4);
  int ret = 0;
  int content_changed = 0;
  int is_elf = n == 4 && memcmp (magic, "\177ELF", 4) == 0;
  int elf_aarch64 = is_elf && elf_is_aarch64 (fd);
  if (is_elf)
    {
      lseek (fd, 0, SEEK_SET);
      unsigned long before_interp = g_n_interp, before_rpath = g_n_rpath;
      ret = relocate_elf (path);
      content_changed = g_n_interp != before_interp
	|| g_n_rpath != before_rpath;
    }
  else if (n >= 2 && magic[0] == '#' && magic[1] == '!')
    ret = relocate_shebang (path, fd);
  else
    g_n_skip++;
  close (fd);

  if (ret == 0)
    {
      /* Root-slot rewriting applies to any regular file with content.  */
      long slots = prelinux_rewrite_root_slots (path, g_oldroot, g_root);
      if (slots > 0)
	{
	  g_n_slots += slots;
	  content_changed = 1;
	  if (!g_quiet)
	    printf ("%s: %ld root slot(s) -> %s\n", path, slots, g_root);
	}
      /* Sign ELFs: when content changed this run, or when
	 the file isn't validly signed yet.  Unmodified+signed files are
	 left alone, keeping relocate byte-idempotent.  Only aarch64 is
	 signed: HarmonyOS PC is aarch64-only, so a signature anywhere
	 else would be inert dead weight.  (`prelinux sign` stays
	 arch-neutral when invoked explicitly.)  */
      if (is_elf && !g_nosign && elf_aarch64)
	{
	  int changed = content_changed;
	  if (changed || !prelinux_signed (path))
	    {
	      if (prelinux_sign_file (path) == 0)
		g_n_signed++;
	      else
		fprintf (stderr, "relocate: %s: signing failed\n", path);
	    }
	}
    }
  return ret;
}

static int g_in_instance; /* running from an installed instance copy */

/* Absolute symlinks dangle against the host's real /: the kernel
   resolves symlink targets itself, and the translation layer only
   rewrites syscall path arguments.  Rewrite targets that fall under a
   mapped prefix to point into the instance root, mirroring interp
   rewriting (the instance is immobile after import either way).
   Relative symlinks, pass-through prefixes and arbitrary host paths
   are left alone.  */
static int
relocate_symlink (const char *path)
{
  char target[PATH_MAX];
  ssize_t n = readlink (path, target, sizeof (target) - 1);
  if (n < 0)
    {
      g_n_err++;
      return -1;
    }
  target[n] = '\0';
  if (target[0] != '/'
      || __prelinux_prefix_match (target, g_root, __prelinux_strlen (g_root)))
    {
      g_n_skip++;
      return 0; /* relative, or already anchored in the instance */
    }

  const char *suffix = NULL;
  if (g_oldroot != NULL
      && __prelinux_prefix_match (target, g_oldroot,
				  __prelinux_strlen (g_oldroot)))
    suffix = target + __prelinux_strlen (g_oldroot); /* moved instance */
  else if (strncmp (target, PRELINUX_ROOT_PLACEHOLDER,
		    __prelinux_strlen (PRELINUX_ROOT_PLACEHOLDER)) == 0)
    suffix = target + __prelinux_strlen (PRELINUX_ROOT_PLACEHOLDER);
  else if (__prelinux_table_match (target, __prelinux_map_table))
    suffix = target; /* FHS-mapped prefix: anchor into the instance */
  else
    {
      g_n_skip++;
      return 0;
    }

  char newt[PATH_MAX];
  snprintf (newt, sizeof (newt), "%s%s", g_root, suffix);
  char tmp[PATH_MAX];
  snprintf (tmp, sizeof (tmp), "%s.prelinux-new", path);
  unlink (tmp);
  if (symlink (newt, tmp) != 0 || rename (tmp, path) != 0)
    {
      fprintf (stderr, "relocate: %s: symlink rewrite failed: %s\n",
	       path, strerror (errno));
      unlink (tmp);
      g_n_err++;
      return -1;
    }
  g_n_links++;
  if (!g_quiet)
    printf ("%s: symlink -> %s\n", path, newt);
  return 0;
}

static int
walk (const char *path_arg)
{
  /* Arguments may be FHS-style (e.g. from the dpkg hook running inside
     an instance) or real paths (packaging, import).  Normalization:
     paths already under the root are literal; FHS-mapped paths are
     translated when running inside the instance, or when no literal
     file by that name exists on the host.  */
  char pathbuf[PATH_MAX];
  const char *path = path_arg;
  if (path_arg[0] == '/'
      && !__prelinux_prefix_match (path_arg, g_root,
				   __prelinux_strlen (g_root)))
    {
      const char *r = __prelinux_translate_path_root (g_root, path_arg,
						      pathbuf,
						      sizeof (pathbuf));
      if (r != NULL && r != path_arg)
	{
	  struct stat lst;
	  if (g_in_instance || lstat (path_arg, &lst) != 0)
	    path = r;
	}
    }

  struct stat st;
  if (lstat (path, &st) != 0)
    {
      fprintf (stderr, "relocate: %s: %s\n", path, strerror (errno));
      g_n_err++;
      return -1;
    }
  if (S_ISDIR (st.st_mode))
    {
      DIR *d = opendir (path);
      if (d == NULL)
	{
	  g_n_err++;
	  return -1;
	}
      int ret = 0;
      struct dirent *de;
      while ((de = readdir (d)) != NULL)
	{
	  if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
	    continue;
	  char sub[PATH_MAX];
	  snprintf (sub, sizeof (sub), "%s/%s", path, de->d_name);
	  if (walk (sub) != 0)
	    ret = -1;
	}
      closedir (d);
      return ret;
    }
  if (S_ISLNK (st.st_mode))
    return relocate_symlink (path);
  if (!S_ISREG (st.st_mode))
    {
      g_n_skip++;
      return 0;
    }
  return relocate_one_real (path, 0);
}

int
prelinux_cmd_relocate (int argc, char **argv)
{
  const char *root_arg = NULL;
  int i = 1;
  while (i < argc && argv[i][0] == '-')
    {
      if (strcmp (argv[i], "--root") == 0 && i + 1 < argc)
	root_arg = argv[++i];
      else if (strcmp (argv[i], "--quiet") == 0)
	g_quiet = 1;
      else if (strcmp (argv[i], "--no-sign") == 0)
	g_nosign = 1;
      else
	{
	  fprintf (stderr, "usage: prelinux relocate [--root DIR] "
			   "[--quiet] [--no-sign] PATH...\n");
	  return 2;
	}
      i++;
    }
  if (i >= argc)
    {
      fprintf (stderr, "usage: prelinux relocate [--root DIR] [--quiet] "
		       "[--no-sign] PATH...\n");
      return 2;
    }

  char rootbuf[PATH_MAX];
  if (prelinux_resolve_root (root_arg, rootbuf, sizeof (rootbuf)) != 0)
    {
      fprintf (stderr, "relocate: cannot determine the instance root; "
		       "pass --root DIR\n");
      return 2;
    }
  g_root = rootbuf;

  /* The old root is this binary's own storage when it has been
     installed before (i.e. we run from inside an existing instance).  */
  /* The root this binary was relocated to is the "from" root for path
     rewriting.  A CLI that still carries the compiled placeholder (the
     one shipped inside an image, which is what `prelinux import` runs)
     must treat the placeholder as that from-root, otherwise PT_INTERP
     and RPATH strings that contain it are left untouched.  */
  g_oldroot = NULL;
  if (prelinux_root_is_placeholder (prelinux_root ()))
    g_oldroot = prelinux_root ();
  else if (strcmp (prelinux_root (), g_root) != 0)
    g_oldroot = prelinux_root ();
  /* The import driver may pass the old root explicitly.  */
  {
    const char *from = getenv ("PRELINUX_RELOCATE_FROM");
    if (from != NULL && from[0] == '/')
      g_oldroot = from;
  }

  g_in_instance = !prelinux_root_is_placeholder (prelinux_root ())
    && strcmp (prelinux_root (), g_root) == 0;

  int ret = 0;
  for (; i < argc; i++)
    if (walk (argv[i]) != 0)
      ret = 1;

  if (!g_quiet)
    printf ("relocate: %lu interp, %lu rpath, %lu shebang, %lu slots, "
	    "%lu symlinks, %lu signed, %lu skipped, %lu errors\n",
	    g_n_interp, g_n_rpath, g_n_shebang, g_n_slots, g_n_links,
	    g_n_signed, g_n_skip, g_n_err);
  return ret || g_n_err ? 1 : 0;
}
