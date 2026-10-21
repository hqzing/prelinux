/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux base SDK — glibc overlay.
   Replace the distro's glibc runtime files with prelinux-glibc (which
   must be at least as new as the distro build).  Content is replaced in
   place under the distro's own file names, so dpkg's file lists and any
   existing ld.so.cache keep resolving.  Used by `import` (right after
   unpacking).  */

#define _GNU_SOURCE 1 /* memmem */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "prelinux.h"

/* soname basenames to overlay; the ld.so name is arch-dependent.  */
static const char *g_overlay_names[64];
static int g_overlay_nnames;
static const char *g_our_ver;      /* our glibc version */
static const char *g_distro_glibc; /* the distro's version, for the manifest */
static const char *g_label = "import";
static int g_quiet;

static void
die (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "%s: ", g_label);
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\n");
  va_end (ap);
  exit (1);
}

static int
is_overlay_name (const char *base)
{
  for (int i = 0; i < g_overlay_nnames; i++)
    if (strcmp (base, g_overlay_names[i]) == 0)
      return 1;
  return 0;
}

/* "2.39" from a glibc DSO by scanning its version banner ("GNU C
   Library ... stable release version 2.39").  Binary scan, not
   execution: at overlay time PT_INTERP may still point at the pristine
   placeholder, and executing foreign DSOs is fragile anyway.  */
static const char *
dso_version (const char *path)
{
  static char ver[64];
  ver[0] = '\0';
  int fd = open (path, O_RDONLY);
  if (fd < 0)
    return ver;
  struct stat st;
  if (fstat (fd, &st) != 0 || st.st_size < 4096)
    {
      close (fd);
      return ver;
    }
  size_t sz = (size_t) st.st_size;
  char *buf = malloc (sz);
  if (buf == NULL || read (fd, buf, sz) != (ssize_t) sz)
    {
      free (buf);
      close (fd);
      return ver;
    }
  close (fd);
  static const char pat[] = "stable release version ";
  char *hit = memmem (buf, sz, pat, sizeof (pat) - 1);
  if (hit != NULL)
    {
      char *p = hit + sizeof (pat) - 1;
      size_t n = 0;
      while (n + 1 < sizeof (ver)
	     && (p[n] == '.' || (p[n] >= '0' && p[n] <= '9')))
	{
	  ver[n] = p[n];
	  n++;
	}
      ver[n] = '\0';
    }
  free (buf);
  return ver;
}

static int
vercmp (const char *a, const char *b)
{
  long amaj = strtol (a, NULL, 10), bmaj = strtol (b, NULL, 10);
  const char *adot = strchr (a, '.'), *bdot = strchr (b, '.');
  long amin = adot ? strtol (adot + 1, NULL, 10) : 0;
  long bmin = bdot ? strtol (bdot + 1, NULL, 10) : 0;
  if (amaj != bmaj)
    return amaj < bmaj ? -1 : 1;
  if (amin != bmin)
    return amin < bmin ? -1 : 1;
  return 0;
}

static int g_sign;
static int g_overlaid;

static void
copy_file (const char *src, const char *dst)
{
  /* Write a fresh inode and rename it over DST.  Truncating DST in
     place would corrupt every process that already has the old library
     mapped: the import pass replaces libc while the importing shell may
     already be using it.  */
  int in = open (src, O_RDONLY);
  if (in < 0)
    die ("cannot open %s", src);
  char tmp[PATH_MAX];
  snprintf (tmp, sizeof (tmp), "%s.prelinux-tmp", dst);
  unlink (tmp);
  int out = open (tmp, O_WRONLY | O_CREAT | O_EXCL, 0755);
  if (out < 0)
    {
      close (in);
      die ("cannot write %s: %s", tmp, strerror (errno));
    }
  char buf[65536];
  ssize_t n;
  while ((n = read (in, buf, sizeof (buf))) > 0)
    if (write (out, buf, (size_t) n) != n)
      die ("write %s failed", dst);
  close (in);
  if (fchmod (out, 0755) != 0)
    {
      close (out);
      unlink (tmp);
      die ("chmod %s failed", tmp);
    }
  close (out);
  if (rename (tmp, dst) != 0)
    {
      /* HarmonyOS makes signed files immutable: the rename over them
	 fails.  Removing first leaves a window with no file, which is
	 fine at import time.  */
      if ((errno == EPERM || errno == EACCES) && unlink (dst) == 0
	  && rename (tmp, dst) == 0)
	return;
      unlink (tmp);
      die ("cannot replace %s: %s", dst, strerror (errno));
    }
}

/* Overlay soname S at instance path P (real file or symlink).  */
static void
overlay_one (const char *instroot, const char *p, const char *s)
{
  char ours[PATH_MAX];
  snprintf (ours, sizeof (ours), "%s/glibc/lib/%s", instroot, s);
  struct stat st;
  if (stat (ours, &st) != 0)
    return; /* our glibc does not ship this soname; leave distro's */

  /* resolve the real file behind P (symlinks) */
  char realp[PATH_MAX];
  char t[256];
  ssize_t n = readlink (p, t, sizeof (t) - 1);
  if (n > 0)
    {
      t[n] = '\0';
      if (t[0] == '/')
	snprintf (realp, sizeof (realp), "%s%s", instroot, t);
      else
	{
	  char dirb[PATH_MAX];
	  snprintf (dirb, sizeof (dirb), "%s", p);
	  char *sl = strrchr (dirb, '/');
	  if (sl)
	    *sl = '\0';
	  snprintf (realp, sizeof (realp), "%s/%s", dirb, t);
	}
    }
  else
    snprintf (realp, sizeof (realp), "%s", p);
  if (stat (realp, &st) != 0)
    snprintf (realp, sizeof (realp), "%s", p); /* dangling: overwrite P */

  const char *theirs = dso_version (realp);
  if (theirs[0] && strcmp (s, "libc.so.6") == 0)
    {
      static char vbuf[64];
      snprintf (vbuf, sizeof (vbuf), "%s", theirs);
      g_distro_glibc = vbuf;
    }
  if (theirs[0] && g_our_ver && g_our_ver[0]
      && vercmp (g_our_ver, theirs) < 0)
    die ("prelinux-glibc %s is older than the distro's %s (%s); "
	 "unsupported combination", g_our_ver, theirs, s);

  copy_file (ours, realp);
  if (g_sign)
    prelinux_sign_file (realp);
  if (!g_quiet)
    printf ("%s: overlay %s (%s -> %s)\n", g_label, s,
	    theirs[0] ? theirs : "?", g_our_ver ? g_our_ver : "?");
  g_overlaid++;
}

static void
overlay_walk (const char *instroot, const char *dir, int depth)
{
  DIR *d = opendir (dir);
  if (d == NULL)
    return;
  char subdirs[64][PATH_MAX];
  int nsub = 0;
  struct dirent *de;
  while ((de = readdir (d)) != NULL)
    {
      if (de->d_name[0] == '.')
	continue;
      char path[PATH_MAX];
      snprintf (path, sizeof (path), "%s/%s", dir, de->d_name);
      struct stat st;
      if (lstat (path, &st) != 0)
	continue;
      if (S_ISDIR (st.st_mode))
	{
	  if (depth < 1 && nsub < 64)
	    {
	      snprintf (subdirs[nsub], PATH_MAX, "%s", path);
	      nsub++;
	    }
	  continue;
	}
      if (is_overlay_name (de->d_name))
	overlay_one (instroot, path, de->d_name);
    }
  closedir (d);
  for (int i = 0; i < nsub; i++)
    overlay_walk (instroot, subdirs[i], depth + 1);
}

int
prelinux_overlay_glibc (const char *root, const char *label, int sign,
			int quiet)
{
  static const char *common[] = {
    "libc.so.6", "libm.so.6", "libmvec.so.1", "libdl.so.2",
    "libpthread.so.0", "librt.so.1", "libutil.so.1", "libresolv.so.2",
    "libanl.so.1", "libBrokenLocale.so.1", "libthread_db.so.1",
    "libnss_files.so.2", "libnss_dns.so.2", "libnss_db.so.2",
    "libnss_compat.so.2", "libnss_hesiod.so.2", "libc_malloc_debug.so.0",
    "libmemusage.so", "libpcprofile.so", "libSegFault.so",
  };
  int n = 0;
  for (size_t i = 0; i < sizeof (common) / sizeof (common[0]); i++)
    g_overlay_names[n++] = common[i];
#ifdef __aarch64__
  g_overlay_names[n++] = "ld-linux-aarch64.so.1";
#else
  g_overlay_names[n++] = "ld-linux-x86-64.so.2";
#endif
  g_overlay_nnames = n;
  g_label = label;
  g_quiet = quiet;
  g_sign = sign;
  g_overlaid = 0;

  {
    static char ourver[64];
    char p[PATH_MAX];
    snprintf (p, sizeof (p), "%s/glibc/lib/libc.so.6", root);
    snprintf (ourver, sizeof (ourver), "%s", dso_version (p));
    g_our_ver = ourver;
    if (ourver[0] == '\0')
      die ("cannot determine prelinux-glibc version from %s", p);
  }

  static const char *libdirs[] = { "/lib", "/lib64", "/usr/lib", NULL };
  for (int i = 0; libdirs[i]; i++)
    {
      char d[PATH_MAX];
      snprintf (d, sizeof (d), "%s%s", root, libdirs[i]);
      overlay_walk (root, d, 0);
    }
  return g_overlaid;
}

/* The distro ships ldconfig statically linked, so a distro upgrade puts
   an ldconfig back that bypasses the translation layer and rewrites the
   *host's* cache.  Replace every copy with ours, which carries the
   translation in its static libc.  Returns the number replaced.  */
int
prelinux_restore_ldconfig (const char *root, int quiet)
{
  static const char *paths[] = { "/sbin/ldconfig", "/sbin/ldconfig.real",
				 "/usr/sbin/ldconfig", "/usr/sbin/ldconfig.real",
				 NULL };
  char src[PATH_MAX];
  snprintf (src, sizeof (src), "%s/glibc/sbin/ldconfig", root);
  struct stat sst;
  if (stat (src, &sst) != 0)
    return 0;
  int n = 0;
  for (int i = 0; paths[i]; i++)
    {
      char dst[PATH_MAX];
      snprintf (dst, sizeof (dst), "%s%s", root, paths[i]);
      struct stat dstst;
      if (stat (dst, &dstst) != 0)
	continue;
      copy_file (src, dst);
      if (g_sign)
	prelinux_sign_file (dst);
      if (!quiet)
	printf ("%s: restore %s\n", g_label, paths[i]);
      n++;
    }
  return n;
}

const char *
prelinux_overlay_distro_glibc (void)
{
  return g_distro_glibc;
}
