/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux base SDK — `prelinux import`.
   Creates an instance from one self-contained image (distro rootfs +
   glibc + toolchain + package manager), fetched
   from our release or given as a local file:

     unpack image -> overlay prelinux-glibc onto the distro's glibc
     files -> relocate the whole tree (PT_INTERP / RPATH / shebang /
     root slots) -> write env.sh and .prelinux/instance.toml -> smoke
     test.

   External tools are spawned, never linked: curl (download), sha256sum
   (verify), gzip/xz/zstd + tar (unpack).  The mapping rules are never
   reimplemented here; path math uses the shared translation core.  */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "prelinux.h"

static void
die (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "import: ");
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\n");
  va_end (ap);
  exit (1);
}

/* -------- subprocess helpers ---------------------------------------- */

static int
run (char *const argv[])
{
  pid_t pid = fork ();
  if (pid == 0)
    {
      execvp (argv[0], argv);
      fprintf (stderr, "import: cannot run %s: %s\n", argv[0],
	       strerror (errno));
      _exit (127);
    }
  int st;
  if (pid < 0 || waitpid (pid, &st, 0) < 0)
    return -1;
  if (!WIFEXITED (st) || WEXITSTATUS (st) != 0)
    return -1;
  return 0;
}

/* Spawn PRODUCER | CONSUMER and wait for both.  */
static int
run_pipe (char *const producer[], char *const consumer[])
{
  int pfd[2];
  if (pipe (pfd) != 0)
    return -1;
  pid_t p1 = fork ();
  if (p1 == 0)
    {
      dup2 (pfd[1], 1);
      close (pfd[0]);
      close (pfd[1]);
      execvp (producer[0], producer);
      _exit (127);
    }
  pid_t p2 = fork ();
  if (p2 == 0)
    {
      dup2 (pfd[0], 0);
      close (pfd[0]);
      close (pfd[1]);
      execvp (consumer[0], consumer);
      _exit (127);
    }
  close (pfd[0]);
  close (pfd[1]);
  int st1, st2;
  if (p1 < 0 || p2 < 0 || waitpid (p1, &st1, 0) < 0
      || waitpid (p2, &st2, 0) < 0)
    return -1;
  if (!WIFEXITED (st1) || WEXITSTATUS (st1) != 0 || !WIFEXITED (st2)
      || WEXITSTATUS (st2) != 0)
    return -1;
  return 0;
}

static int
capture (char *const argv[], char *out, size_t outlen)
{
  int pfd[2];
  if (pipe (pfd) != 0)
    return -1;
  pid_t pid = fork ();
  if (pid == 0)
    {
      dup2 (pfd[1], 1);
      close (pfd[0]);
      close (pfd[1]);
      execvp (argv[0], argv);
      _exit (127);
    }
  close (pfd[1]);
  /* drain until EOF: closing early would SIGPIPE-kill the child */
  size_t off = 0;
  for (;;)
    {
      ssize_t n = read (pfd[0], out + off, outlen - 1 - off);
      if (n <= 0)
	break;
      off += (size_t) n;
      if (off >= outlen - 1)
	{
	  /* output longer than the buffer: keep draining */
	  char junk[4096];
	  while (read (pfd[0], junk, sizeof (junk)) > 0)
	    ;
	  break;
	}
    }
  close (pfd[0]);
  int st;
  waitpid (pid, &st, 0);
  out[off] = '\0';
  if (!WIFEXITED (st) || WEXITSTATUS (st) != 0)
    return -1;
  return 0;
}

static int
tool_exists (const char *tool)
{
  char *const argv[] = { (char *) tool, (char *) "--version", NULL };
  int rc = capture (argv, (char[256]){0}, 256);
  if (rc == 0)
    return 1;
  /* Some tools print to stderr or lack --version; try running plainly. */
  char *const argv2[] = { (char *) tool, NULL };
  pid_t pid = fork ();
  if (pid == 0)
    {
      int nullfd = open ("/dev/null", O_RDWR);
      dup2 (nullfd, 0);
      dup2 (nullfd, 1);
      dup2 (nullfd, 2);
      execvp (argv2[0], argv2);
      _exit (127);
    }
  int st;
  waitpid (pid, &st, 0);
  return WIFEXITED (st) && WEXITSTATUS (st) != 127;
}

/* -------- distro table (supported distro matrix) --------
   The image (fully self-contained: rootfs + glibc + toolchain + pm) is
   downloaded from OUR release, not from the distro's site (appendix
   the distro table only names the image to fetch.  */

struct distro
{
  const char *name;   /* "ubuntu:24.04" */
  const char *id;     /* "ubuntu2404" */
  const char *arch;   /* "aarch64" / "x86_64" */
  const char *pm;     /* adapter id: "dpkg" | "rpm" */
};

static const char *
host_arch (void)
{
#ifdef __aarch64__
  return "aarch64";
#elif defined (__x86_64__)
  return "x86_64";
#else
# error "unsupported build arch"
#endif
}

static const struct distro g_distros[] = {
  { "ubuntu:24.04", "ubuntu2404", "aarch64", "dpkg" },
  { "ubuntu:24.04", "ubuntu2404", "x86_64", "dpkg" },
  { "debian:12", "debian12", "aarch64", "dpkg" },
  { "debian:12", "debian12", "x86_64", "dpkg" },
  { "rocky:10", "rocky10", "aarch64", "rpm" },
  { "rocky:10", "rocky10", "x86_64", "rpm" },
  { NULL, NULL, NULL, NULL }
};

static const struct distro *
find_distro (const char *name)
{
  for (int i = 0; g_distros[i].name; i++)
    if (strcmp (g_distros[i].name, name) == 0
	&& strcmp (g_distros[i].arch, host_arch ()) == 0)
      return &g_distros[i];
  return NULL;
}

/* -------- download / verify / unpack -------------------------------- */

static int
download_soft (const char *url, const char *out)
{
  char *const argv[] = { "curl", "-fSL", "--retry", "3", "-o",
			 (char *) out, (char *) url, NULL };
  return run (argv);
}

/* Read the expected sha256 for NAME from a SHA256SUMS file.  */
static int
sum_from_file (const char *sumsfile, const char *name, char out[65])
{
  FILE *f = fopen (sumsfile, "r");
  if (f == NULL)
    return -1;
  char line[512];
  int found = 0;
  while (fgets (line, sizeof (line), f) != NULL)
    {
      size_t l = strlen (line);
      while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
	line[--l] = '\0';
      if (l < 66)
	continue;
      const char *p = line + 64;
      while (*p == ' ')
	p++;
      if (strcmp (p, name) == 0)
	{
	  memcpy (out, line, 64);
	  out[64] = '\0';
	  found = 1;
	  break;
	}
    }
  fclose (f);
  return found ? 0 : -1;
}

static void verify_sha256 (const char *file, const char *want);

/* Fetch the self-contained image for distro D into the cache, verifying
   against the release's SHA256SUMS.  Release bases: env override
   $PRELINUX_RELEASE_URL, else our GitHub release's latest download.  */
static void
fetch_image (const struct distro *d, const char *cache, char *out,
	     size_t outlen)
{
  char name[128];
  snprintf (name, sizeof (name), "prelinux-image-%s-%s", d->id,
	    host_arch ());

  const char *bases[2];
  int nb = 0;
  const char *env = getenv ("PRELINUX_RELEASE_URL");
  if (env != NULL && *env != '\0')
    bases[nb++] = env;
  else
    bases[nb++] =
      "https://github.com/hqzing/prelinux/releases/latest/download";
  bases[nb] = NULL;

  const char *fmts[] = { "tar.zst", "tar.gz", NULL };
  int f0 = tool_exists ("zstd") ? 0 : 1;

  char url[PATH_MAX], sumfile[PATH_MAX], want[65];
  for (int b = 0; bases[b]; b++)
    {
      snprintf (sumfile, sizeof (sumfile), "%s/SHA256SUMS", cache);
      snprintf (url, sizeof (url), "%s/SHA256SUMS", bases[b]);
      if (download_soft (url, sumfile) != 0)
	continue;
      for (int f = f0; fmts[f]; f++)
	{
	  char fname[160];
	  snprintf (fname, sizeof (fname), "%s.%s", name, fmts[f]);
	  if (sum_from_file (sumfile, fname, want) != 0)
	    continue;
	  snprintf (out, outlen, "%s/%s", cache, fname);
	  struct stat st;
	  if (stat (out, &st) != 0)
	    {
	      snprintf (url, sizeof (url), "%s/%s", bases[b], fname);
	      printf ("import: downloading %s\n", url);
	      if (download_soft (url, out) != 0)
		continue;
	    }
	  verify_sha256 (out, want);
	  return;
	}
    }
  /* Never trust an unverified cache: the release's SHA256SUMS is the
     contract (design 11.1).  When no base answers, point at the file the
     cache may already hold so the user can import it explicitly (which
     is the documented local-image path) instead of silently skipping
     verification.  */
  {
    char hint[PATH_MAX];
    hint[0] = '\0';
    for (int f = 0; fmts[f]; f++)
      {
	char fname[160];
	snprintf (fname, sizeof (fname), "%s.%s", name, fmts[f]);
	snprintf (hint, sizeof (hint), "%s/%s", cache, fname);
	struct stat st;
	if (stat (hint, &st) == 0 && st.st_size > 0)
	  die ("cannot fetch %s from any release base (set "
	       "PRELINUX_RELEASE_URL); a copy exists at %s — import it "
	       "explicitly with `prelinux import %s <dir>` if you trust it",
	       name, hint, hint);
      }
  }
  die ("cannot fetch %s from any release base (set PRELINUX_RELEASE_URL "
       "or pass a local image path)", name);
}

static void
verify_sha256 (const char *file, const char *want)
{
  if (want == NULL)
    {
      fprintf (stderr, "import: warning: no pinned sha256 for %s\n", file);
      return;
    }
  char out[256];
  char *const argv[] = { "sha256sum", (char *) file, NULL };
  if (capture (argv, out, sizeof (out)) != 0)
    die ("sha256sum failed for %s", file);
  if (strncasecmp (out, want, 64) != 0)
    die ("sha256 mismatch for %s (got %.64s)", file, out);
}

/* GNU tar applies recorded directory permissions eagerly and its
   in-place overwrite trips on read-only files and existing symlinks
   (the image overlays the distro rootfs: rpm's hardlinked 0555 tools
   reproduce this as a non-root user); --overwrite plus
   --no-same-permissions --delay-directory-restore avoid the class.
   Toybox tar (dockerharmony) rejects those flags, so probe once.  */
static int
gnu_tar (void)
{
  static int gnu = -1;
  if (gnu < 0)
    {
      char out[256];
      char *const argv[] = { (char *) "tar", (char *) "--version", NULL };
      gnu = capture (argv, out, sizeof (out)) == 0
	    && strstr (out, "GNU") != NULL;
    }
  return gnu;
}

static int
untar (const char *file, const char *dir)
{
  char *argv[16];
  int i = 0;
  argv[i++] = (char *) "tar";
  argv[i++] = (char *) "-x";
  if (gnu_tar ())
    {
      argv[i++] = (char *) "--overwrite";
      argv[i++] = (char *) "--no-same-permissions";
      argv[i++] = (char *) "--delay-directory-restore";
    }
  argv[i++] = (char *) "-C";
  argv[i++] = (char *) dir;
  argv[i++] = (char *) "-f";
  argv[i++] = (char *) file;
  argv[i] = NULL;
  return run (argv);
}

/* Force the unpacked tree owner-writable (dirs u+rwx, files u+w):
   read-only entries shipped by some distros would otherwise break the
   image overlay and any later non-root package operation.  Symlinks are
   left alone.  */
static void
make_owner_writable (const char *dir)
{
  DIR *d = opendir (dir);
  if (d == NULL)
    return;
  struct dirent *de;
  while ((de = readdir (d)) != NULL)
    {
      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
	continue;
      char p[PATH_MAX];
      snprintf (p, sizeof (p), "%s/%s", dir, de->d_name);
      struct stat st;
      if (lstat (p, &st) != 0)
	continue;
      if (S_ISDIR (st.st_mode))
	{
	  if ((st.st_mode & 0700) != 0700)
	    chmod (p, (st.st_mode & 07777) | 0700);
	  make_owner_writable (p);
	}
      else if (S_ISREG (st.st_mode) && !(st.st_mode & 0200))
	chmod (p, (st.st_mode & 07777) | 0200);
    }
  closedir (d);
}

static void
unpack_tarball (const char *file, const char *dir)
{
  unsigned char magic[8];
  int fd = open (file, O_RDONLY);
  if (fd < 0 || read (fd, magic, 8) != 8)
    die ("cannot read %s", file);
  close (fd);

  const char *dec = NULL;
  if (magic[0] == 0x1f && magic[1] == 0x8b)
    dec = "gzip";
  else if (magic[0] == 0xfd && magic[1] == '7')
    dec = "xz";
  else if (magic[0] == 0x28 && magic[1] == 0xb5)
    dec = "zstd";
  /* else: plain tar */
  if (dec != NULL && !tool_exists (dec))
    die ("%s needs the %s tool, which is not installed on this host",
	 file, dec);

  if (dec == NULL)
    {
      if (untar (file, dir) != 0)
	die ("tar unpack failed: %s", file);
    }
  else
    {
      char *const prod[] = { (char *) dec, (char *) "-dc", (char *) file,
			     NULL };
      char *cons[16];
      int i = 0;
      cons[i++] = (char *) "tar";
      cons[i++] = (char *) "-x";
      if (gnu_tar ())
	{
	  cons[i++] = (char *) "--overwrite";
	  cons[i++] = (char *) "--no-same-permissions";
	  cons[i++] = (char *) "--delay-directory-restore";
	}
      cons[i++] = (char *) "-C";
      cons[i++] = (char *) dir;
      cons[i++] = (char *) "-f";
      cons[i++] = (char *) "-";
      cons[i] = NULL;
      if (run_pipe (prod, cons) != 0)
	die ("unpack pipeline failed: %s", file);
    }
}


/* -------- metadata --------------------------------------------------- */

static void
write_file (const char *path, const char *content)
{
  int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || write (fd, content, strlen (content)) < 0)
    die ("cannot write %s", path);
  close (fd);
}

static void
write_env_sh (const char *root, const char *name)
{
  char *tmpl =
    "# PreLinux instance environment — source this file.\n"
    "# The path mapping itself never reads PRELINUX_ROOT; it is\n"
    "# informational only (the instance root is compiled into glibc).\n"
    "PRELINUX_ROOT='%s'\n"
    "export PRELINUX_ROOT\n"
    "PRELINUX_RELOCATE='%s/prelinux/bin/prelinux'\n"
    "export PRELINUX_RELOCATE\n"
    "case \":$PATH:\" in\n"
    "  *\":$PRELINUX_ROOT/bin:\"*) ;;\n"
    "  *) PATH=\"$PRELINUX_ROOT/bin:$PRELINUX_ROOT/usr/bin:$PRELINUX_ROOT/sbin:$PRELINUX_ROOT/usr/sbin:$PRELINUX_ROOT/prelinux/bin:$PATH\" ;;\n"
    "esac\n"
    "export PATH\n"
    "PS1='(prelinux:%s) '${PS1:-'\\\\$ '}\n"
    "prelinux-shell() { env PS1='(prelinux:%s) $ ' \"$PRELINUX_ROOT/bin/sh\" -i; }\n";
  char content[8192];
  snprintf (content, sizeof (content), tmpl, root, root, name, name);
  char path[PATH_MAX];
  snprintf (path, sizeof (path), "%s/env.sh", root);
  write_file (path, content);
}

static void
write_instance_toml (const char *root, const char *name,
		     const char *distro, const char *version,
		     const char *pm)
{
  char dir[PATH_MAX];
  snprintf (dir, sizeof (dir), "%s/.prelinux", root);
  mkdir (dir, 0755);
  char path[PATH_MAX];
  snprintf (path, sizeof (path), "%s/instance.toml", dir);
  time_t now = time (NULL);
  char ts[64];
  strftime (ts, sizeof (ts), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
  char content[2048];
  snprintf (content, sizeof (content),
	    "# PreLinux instance metadata\n"
	    "name = \"%s\"\n"
	    "distro = \"%s\"\n"
	    "version = \"%s\"\n"
	    "arch = \"%s\"\n"
	    "adapter = \"%s\"\n"
	    "glibc = \"2.39\"\n"
	    "distro_glibc = \"%s\"\n"
	    "created = \"%s\"\n"
	    "root = \"%s\"\n",
	    name, distro, version, host_arch (), pm ? pm : "none",
	    prelinux_overlay_distro_glibc () ? prelinux_overlay_distro_glibc () : "unknown", ts, root);
  write_file (path, content);
  snprintf (path, sizeof (path), "%s/compat.log", dir);
  int fd = open (path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0)
    {
      dprintf (fd, "# %s instance %s created (%s %s)\n", ts, name, distro,
	       version);
      close (fd);
    }
}

/* -------- smoke test -------------------------------------------------- */

static void
smoke_test (const char *root)
{
  char sh[PATH_MAX];
  snprintf (sh, sizeof (sh), "%s/bin/sh", root);
  char path[PATH_MAX];
  snprintf (path, sizeof (path),
	    "%s/bin:%s/usr/bin:%s/sbin:%s/usr/sbin:%s/prelinux/bin",
	    root, root, root, root, root);
  char out[1024];
  out[0] = '\0';
  int pfd[2];
  if (pipe (pfd) != 0)
    die ("pipe: %s", strerror (errno));
  pid_t pid = fork ();
  if (pid == 0)
    {
      dup2 (pfd[1], 1);
      close (pfd[0]);
      close (pfd[1]);
      setenv ("PATH", path, 1);
      execl (sh, "sh", "-c",
	     "echo PRELINUX_SMOKE_OK; head -1 /etc/os-release 2>/dev/null",
	     (char *) NULL);
      _exit (127);
    }
  close (pfd[1]);
  size_t off = 0;
  ssize_t n;
  while (off < sizeof (out) - 1
	 && (n = read (pfd[0], out + off, sizeof (out) - 1 - off)) > 0)
    off += (size_t) n;
  close (pfd[0]);
  out[off] = '\0';
  int st;
  waitpid (pid, &st, 0);
  if (!WIFEXITED (st) || WEXITSTATUS (st) != 0
      || strstr (out, "PRELINUX_SMOKE_OK") == NULL)
    die ("smoke test failed (exit %d): %s",
	 WIFEXITED (st) ? WEXITSTATUS (st) : -1, out);
  printf ("import: smoke test passed: %s", out);
}

/* -------- main -------------------------------------------------------- */

static void
usage (void)
{
  fprintf (stderr,
	   "usage: prelinux import <distro:ver|image.tar[.gz|.zst]> [dir]\n"
	   "supported distros: debian:12 ubuntu:24.04 rocky:10\n"
	   "downloads land in <install root>/cache; override the release base "
	   "with $PRELINUX_RELEASE_URL\n");
  exit (2);
}


/* Kick off the distribution's own identity file.  /etc/os-release is
   standard, and on usr-merged systems it is a symlink to
   /usr/lib/os-release, so both paths are tried.  Returns 1 when ID was
   found.  */
static int
read_os_release (const char *dir, char *id, size_t idlen, char *ver,
		 size_t verlen)
{
  static const char *paths[] = { "/etc/os-release", "/usr/lib/os-release",
				 NULL };
  char path[PATH_MAX], line[512];
  id[0] = '\0';
  ver[0] = '\0';
  for (int i = 0; paths[i]; i++)
    {
      snprintf (path, sizeof (path), "%s%s", dir, paths[i]);
      FILE *f = fopen (path, "r");
      if (f == NULL)
	continue;
      while (fgets (line, sizeof (line), f) != NULL)
	{
	  char *p = line;
	  while (*p == ' ' || *p == '\t')
	    p++;
	  if (strncmp (p, "ID=", 3) == 0)
	    {
	      char *v = p + 3;
	      char *q = strchr (v, '"');
	      if (q != NULL)
		{
		  char *r = strchr (q + 1, '"');
		  if (r != NULL && (size_t) (r - q - 1) < idlen)
		    {
		      memcpy (id, q + 1, (size_t) (r - q - 1));
		      id[r - q - 1] = '\0';
		    }
		}
	      else if (strlen (v) + 1 <= idlen)
		snprintf (id, idlen, "%s", v);
	      if (id[0] != '\0')
		{
		  size_t l = strlen (id);
		  while (l > 0 && (id[l - 1] == '\n' || id[l - 1] == ' '))
		    id[--l] = '\0';
		}
	    }
	  else if (strncmp (p, "VERSION_ID=", 11) == 0)
	    {
	      char *v = p + 11;
	      char *q = strchr (v, '"');
	      if (q != NULL)
		{
		  char *r = strchr (q + 1, '"');
		  if (r != NULL && (size_t) (r - q - 1) < verlen)
		    {
		      memcpy (ver, q + 1, (size_t) (r - q - 1));
		      ver[r - q - 1] = '\0';
		    }
		}
	      else if (strlen (v) + 1 <= verlen)
		snprintf (ver, verlen, "%s", v);
	      if (ver[0] != '\0')
		{
		  size_t l = strlen (ver);
		  while (l > 0 && (ver[l - 1] == '\n' || ver[l - 1] == ' '))
		    ver[--l] = '\0';
		}
	    }
	}
      fclose (f);
      if (id[0] != '\0')
	return 1;
    }
  return 0;
}

/* Map the identity file onto the distro table: the same ID and a
   version the table's entry starts with (Rocky says "10.0", the table
   says "10").  Returns the matching entry or NULL.  */
static const struct distro *
detect_distro (const char *dir, char *ver, size_t verlen)
{
  char id[64];
  if (!read_os_release (dir, id, sizeof (id), ver, verlen))
    return NULL;
  for (int i = 0; g_distros[i].name; i++)
    {
      const char *colon = strchr (g_distros[i].name, ':');
      if (colon == NULL)
	continue;
      size_t idl = (size_t) (colon - g_distros[i].name);
      if (strlen (id) != idl || strncmp (id, g_distros[i].name, idl) != 0)
	continue;
      const char *v = colon + 1;
      if (strncmp (ver, v, strlen (v)) == 0)
	return &g_distros[i];
    }
  return NULL;
}

/* Which package manager the tree carries, for instance metadata.  */
static const char *
detect_pm (const char *dir)
{
  static const char *dnf[] = { "/usr/bin/dnf", "/bin/dnf", NULL };
  static const char *dpkg[] = { "/usr/bin/dpkg", "/bin/dpkg", NULL };
  char p[PATH_MAX];
  struct stat st;
  for (int i = 0; dnf[i]; i++)
    {
      snprintf (p, sizeof (p), "%s%s", dir, dnf[i]);
      if (stat (p, &st) == 0)
	return "rpm";
    }
  for (int i = 0; dpkg[i]; i++)
    {
      snprintf (p, sizeof (p), "%s%s", dir, dpkg[i]);
      if (stat (p, &st) == 0)
	return "dpkg";
    }
  return NULL;
}

/* Keep dnf from upgrading the overlaid glibc.  dnf.conf is an ini file
   with a single [main] section; the option is a space-separated list.
   The file is copied line by line so unrelated settings and comments
   survive, and rewritten through a temporary file.  */
static void
add_dnf_exclude (const char *dir)
{
  /* One glob: glibc-devel and the langpacks also demand an exact glibc
     version, so excluding only `glibc` leaves `dnf upgrade` unable to
     solve the transaction at all.  */
  static const char ex[] = "glibc*";
  char conf[PATH_MAX], tmp[PATH_MAX];
  snprintf (conf, sizeof (conf), "%s/etc/dnf/dnf.conf", dir);
  snprintf (tmp, sizeof (tmp), "%s.tmp", conf);

  int has_main = 0, has_exclude = 0, covers = 0, existed = 0;
  for (int pass = 0; pass < 2; pass++)
    {
      FILE *in = fopen (conf, "r");
      if (pass == 0)
	{
	  if (in == NULL)
	    break; /* no config yet: written below */
	  existed = 1;
	}
      char line[1024];
      FILE *out = NULL;
      if (pass == 1)
	{
	  out = fopen (tmp, "w");
	  if (out == NULL)
	    {
	      fprintf (stderr, "import: warning: cannot write %s\n", tmp);
	      if (in != NULL)
		fclose (in);
	      return;
	    }
	  if (!has_main)
	    fprintf (out, "[main]\nexclude=%s\n", ex);
	}
      while (in != NULL && fgets (line, sizeof (line), in) != NULL)
	{
	  char *p = line;
	  while (*p == ' ' || *p == '\t')
	    p++;
	  int is_main = strncmp (p, "[main]", 6) == 0;
	  int is_excl = strncmp (p, "exclude", 7) == 0
			&& (p[7] == '=' || p[7] == ' ' || p[7] == '\t');
	  if (pass == 0)
	    {
	      has_main |= is_main;
	      if (is_excl)
		{
		  has_exclude = 1;
		  if (strstr (p, "glibc") != NULL)
		    covers = 1;
		}
	      continue;
	    }
	  /* pass 1: write */
	  size_t l = strlen (line);
	  while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
	    line[--l] = '\0';
	  if (is_excl && !covers)
	    fprintf (out, "%s %s\n", line, ex);
	  else
	    fprintf (out, "%s\n", line);
	  if (is_main && !has_exclude)
	    fprintf (out, "exclude=%s\n", ex);
	}
      if (in != NULL)
	fclose (in);
      if (out != NULL)
	fclose (out);
    }
  if (!existed)
    {
      FILE *fp = fopen (conf, "w");
      if (fp != NULL)
	{
	  fprintf (fp, "[main]\nexclude=%s\n", ex);
	  fclose (fp);
	}
      return;
    }
  if (rename (tmp, conf) != 0)
    fprintf (stderr, "import: warning: could not update %s\n", conf);
}

int
prelinux_cmd_import (int argc, char **argv)
{
  const char *source = NULL, *dir = NULL;
  int i = 1;
  for (; i < argc; i++)
    {
      if (source == NULL)
	source = argv[i];
      else if (dir == NULL)
	dir = argv[i];
      else
	usage ();
    }
  if (source == NULL)
    usage ();

  const struct distro *d = find_distro (source);
  int local = strchr (source, '/') != NULL || strstr (source, ".tar") != NULL;
  if (!local && d == NULL)
    die ("unknown distro %s for arch %s (supported: debian:12 "
	 "ubuntu:24.04 rocky:10); or pass a local image path", source,
	 host_arch ());

  /* Install root: $PRELINUX_HOME when set (install.sh exports it), else
     ~/prelinux.  Cache and default instance directories hang off it.  */
  char prefix[PATH_MAX];
  const char *ph = getenv ("PRELINUX_HOME");
  if (ph != NULL && ph[0] != '/')
    die ("PRELINUX_HOME must be an absolute path: %s", ph);
  if (ph != NULL)
    snprintf (prefix, sizeof (prefix), "%s", ph);
  else
    {
      const char *home = getenv ("HOME");
      if (home == NULL)
	home = "/root";
      snprintf (prefix, sizeof (prefix), "%s/prelinux", home);
    }

  char dirbuf[PATH_MAX];
  if (dir == NULL)
    {
      const char *id = d ? d->id : "custom";
      snprintf (dirbuf, sizeof (dirbuf), "%s/%s", prefix, id);
      dir = dirbuf;
    }
  if (dir[0] != '/')
    die ("instance dir must be an absolute path: %s", dir);
  /* Two kernel limits bound the root length: the shebang line
     (BINPRM_BUF_SIZE, 256 bytes) and PT_INTERP (the same buffer, hence
     PRELINUX_INTERP_MAX).  The interpreter path is the tighter one:
     root + "/glibc/lib/<ld.so>".  Reject early with the limit
     spelled out — a too-deep root produces binaries that the kernel
     refuses to execute (ENOMEM on HarmonyOS PC, silent truncation on
     Linux).  */
  {
    const char *ldso_suffix =
#ifdef __aarch64__
      "/glibc/lib/ld-linux-aarch64.so.1";
#else
      "/glibc/lib/ld-linux-x86-64.so.2";
#endif
    size_t maxroot = PRELINUX_INTERP_MAX - strlen (ldso_suffix);
    if (maxroot > 240)
      maxroot = 240; /* shebang limit is the next tightest */
    if (strlen (dir) > maxroot)
      die ("instance dir too deep (%zu bytes > %zu): the kernel caps "
	   "the PT_INTERP path at %d bytes (root + '%s'); choose a "
	   "shallower instance root",
	   strlen (dir), maxroot, PRELINUX_INTERP_MAX, ldso_suffix);
  }

  struct stat st;
  if (stat (dir, &st) == 0)
    die ("instance dir already exists: %s (remove it to recreate)", dir);

  /* 1. the image (fully self-contained: distro rootfs + glibc +
     toolchain + package manager).  A local file
     is imported as-is; a distro name fetches from our release.  */
  char cache[PATH_MAX];
  snprintf (cache, sizeof (cache), "%s/cache", prefix);
  char *mkdir_argv[] = { "mkdir", "-p", cache, NULL };
  run (mkdir_argv);

  char imgtar[PATH_MAX];
  if (local)
    snprintf (imgtar, sizeof (imgtar), "%s", source);
  else
    fetch_image (d, cache, imgtar, sizeof (imgtar));

  char *mkdir2[] = { "mkdir", "-p", (char *) dir, NULL };
  run (mkdir2);

  printf ("import: unpacking image -> %s\n", dir);
  unpack_tarball (imgtar, dir);

  /* Read-only dirs shipped in an image (RHEL-family /usr/bin is 0555)
     would break later package operations for non-root users; force the
     tree owner-writable unconditionally.  */
  make_owner_writable (dir);

  /* Identify the tree from the distribution's own file.  A plain rootfs
     tarball without one stays "custom".  */
  char detected_ver[64];
  detected_ver[0] = '\0';
  const struct distro *detected = detect_distro (dir, detected_ver,
						 sizeof (detected_ver));
  const char *detected_pm = detect_pm (dir);

  /* 3. skeleton dirs that some rootfs tarballs omit */
  static const char *skel[] = { "/bin", "/sbin", "/lib", "/lib64", "/usr",
				"/etc", "/var", "/opt", "/tmp", "/home",
				"/root", "/run", "/srv", NULL };
  for (int j = 0; skel[j]; j++)
    {
      char p[PATH_MAX];
      snprintf (p, sizeof (p), "%s%s", dir, skel[j]);
      mkdir (p, 0755);
    }
  {
    char p[PATH_MAX];
    snprintf (p, sizeof (p), "%s/tmp", dir);
    chmod (p, 01777);
  }

  /* 4. overlay prelinux-glibc onto the distro's glibc files */
  prelinux_overlay_glibc (dir, "import", 0, 0);

  /* 4b. ldconfig is statically linked (upstream default), so a
     distro's ldconfig would bypass the translation layer and operate
     on the host's /etc.  Replace every instance copy with ours.  */
  prelinux_restore_ldconfig (dir, 0);

  /* 5. relocate the whole tree (interp/rpath/shebang/root slots) */
  {
    char *const argv[] = { (char *) "prelinux", (char *) "relocate",
			   (char *) "--root", (char *) dir,
			   (char *) "--quiet", (char *) dir, NULL };
    /* Re-exec ourselves so relocate works with its own machinery. */
    pid_t pid = fork ();
    if (pid == 0)
      {
	execv ("/proc/self/exe", argv);
	_exit (127);
      }
    int st2;
    waitpid (pid, &st2, 0);
    if (!WIFEXITED (st2) || WEXITSTATUS (st2) != 0)
      die ("relocate pass failed");
  }

  /* 6. seed DNS/hosts from the host when absent or empty (docker
     exports carry an empty /etc/resolv.conf) */
  {
    char p[PATH_MAX];
    snprintf (p, sizeof (p), "%s/etc/resolv.conf", dir);
    /* ubuntu-base ships resolv.conf as a dangling symlink; a plain cp
       onto it would fail or write through, so unlink first.  */
    snprintf (p, sizeof (p), "%s/etc/resolv.conf", dir);
    if (lstat (p, &st) == 0 && S_ISLNK (st.st_mode))
      {
	struct stat tst;
	if (stat (p, &tst) != 0)
	  unlink (p);
      }
    if (stat (p, &st) != 0 || st.st_size == 0)
      {
	char *const cp[] = { "cp", "-L", "/etc/resolv.conf", p, NULL };
	if (run (cp) != 0)
	  fprintf (stderr, "import: warning: could not seed resolv.conf\n");
      }
    snprintf (p, sizeof (p), "%s/etc/hosts", dir);
    if (stat (p, &st) != 0 || st.st_size == 0)
      {
	char *const cp[] = { "cp", "-L", "/etc/hosts", p, NULL };
	if (run (cp) != 0)
	  fprintf (stderr, "import: warning: could not seed hosts\n");
      }
  }

  /* 6b. pin the distro's libc6 package so a later `apt upgrade` cannot
     replace the overlaid glibc (ld.so and libc must stay in lockstep;
     dpkg --set-selections needs no apt cache.  */
  {
    char dp[PATH_MAX];
    snprintf (dp, sizeof (dp), "%s/usr/bin/dpkg", dir);
    if (stat (dp, &st) == 0)
      {
	char path[PATH_MAX];
	snprintf (path, sizeof (path),
		  "%s/bin:%s/usr/bin:%s/sbin:%s/usr/sbin:%s/prelinux/bin",
		  dir, dir, dir, dir, dir);
	int pfd[2];
	if (pipe (pfd) != 0)
	  die ("pipe: %s", strerror (errno));
	pid_t pid = fork ();
	if (pid == 0)
	  {
	    dup2 (pfd[0], 0);
	    close (pfd[0]);
	    close (pfd[1]);
	    setenv ("PATH", path, 1);
	    setenv ("DEBIAN_FRONTEND", "noninteractive", 1);
	    execl (dp, "dpkg", "--set-selections", (char *) NULL);
	    _exit (127);
	  }
	close (pfd[0]);
	static const char sel[] = "libc6 hold\nlibc6-dev hold\nlibc-bin hold\n";
	write (pfd[1], sel, sizeof (sel) - 1);
	close (pfd[1]);
	int st2;
	waitpid (pid, &st2, 0);
	if (!WIFEXITED (st2) || WEXITSTATUS (st2) != 0)
	  fprintf (stderr, "import: warning: libc6 hold failed\n");
      }
  }

  /* 6c. the rpm side has no counterpart of dpkg's hold.  dnf takes
     `exclude` from dnf.conf, and without it a plain `dnf upgrade`
     replaces the overlaid glibc, which silently switches path
     translation off.  */
  {
    char dnf[PATH_MAX];
    snprintf (dnf, sizeof (dnf), "%s/usr/bin/dnf", dir);
    if (stat (dnf, &st) == 0)
      add_dnf_exclude (dir);
  }

  /* 7. metadata */
  const char *name = strrchr (dir, '/');
  name = name ? name + 1 : dir;
  write_env_sh (dir, name);
  char distrobuf[64], verbuf[64];
  distrobuf[0] = verbuf[0] = '\0';
  if (d != NULL)
    {
      const char *colon = strchr (d->name, ':');
      size_t dl = colon ? (size_t) (colon - d->name) : strlen (d->name);
      if (dl >= sizeof (distrobuf))
	dl = sizeof (distrobuf) - 1;
      memcpy (distrobuf, d->name, dl);
      distrobuf[dl] = '\0';
      snprintf (verbuf, sizeof (verbuf), "%s", colon ? colon + 1 : "");
    }
  else if (detected != NULL)
    {
      /* The table entry is the release line the user picked, so its
	 version is what the metadata records.  The tree's own point
	 release stays readable in /etc/os-release.  */
      const char *colon = strchr (detected->name, ':');
      size_t dl = colon ? (size_t) (colon - detected->name)
	: strlen (detected->name);
      if (dl >= sizeof (distrobuf))
	dl = sizeof (distrobuf) - 1;
      memcpy (distrobuf, detected->name, dl);
      distrobuf[dl] = '\0';
      snprintf (verbuf, sizeof (verbuf), "%s", colon ? colon + 1 : "");
    }
  else
    {
      snprintf (distrobuf, sizeof (distrobuf), "custom");
      snprintf (verbuf, sizeof (verbuf), "%s", detected_ver);
    }
  write_instance_toml (dir, name, distrobuf, verbuf,
		       d ? d->pm : (detected_pm ? detected_pm : NULL));

  /* 8. smoke test */
  smoke_test (dir);

  printf ("\nimport: instance ready at %s\n", dir);
  printf ("next:  source %s/env.sh\n", dir);
  return 0;
}
