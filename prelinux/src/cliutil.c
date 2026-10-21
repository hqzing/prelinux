/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux CLI helpers: root resolution + signing glue (CLI only;
   never linked into libprelinux.so).  */
#define _GNU_SOURCE 1
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRELINUX_STANDALONE 1
#include "prelinux-translate.h"
#include "prelinux.h"
#include "selfsign.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
prelinux_resolve_root (const char *explicit_root, char *out, size_t outlen)
{
  const char *cand = explicit_root;
  if (cand == NULL)
    cand = getenv ("PRELINUX_ROOT");
  if (cand != NULL)
    {
      size_t l = strlen (cand);
      while (l > 1 && cand[l - 1] == '/')
	l--;
      if (l == 0 || cand[0] != '/' || l + 1 > outlen)
	return -1;
      memcpy (out, cand, l);
      out[l] = '\0';
      return 0;
    }

  /* probe our own executable: <root>/prelinux/bin/prelinux */
  char exe[PATH_MAX];
  ssize_t n = readlink ("/proc/self/exe", exe, sizeof (exe) - 1);
  if (n > 0 && n < (ssize_t) sizeof (exe) - 1)
    {
      exe[n] = '\0';
      char *s = strrchr (exe, '/');
      if (s != NULL)
	{
	  *s = '\0'; /* <root>/prelinux/bin */
	  s = strrchr (exe, '/');
	  if (s != NULL)
	    {
	      *s = '\0'; /* <root>/prelinux */
	      size_t el = strlen (exe);
	      if (el > 9 && strcmp (exe + el - 9, "/prelinux") == 0)
		{
		  exe[el - 9] = '\0'; /* <root> */
		  char probe[PATH_MAX];
		  snprintf (probe, sizeof (probe), "%s/glibc/lib", exe);
		  struct stat st;
		  if (strlen (exe) + 1 <= outlen && stat (probe, &st) == 0)
		    {
		      snprintf (out, outlen, "%s", exe);
		      return 0;
		    }
		}
	    }
	}
    }

  /* compiled-in storage (only valid after installation-time rewrite) */
  if (!prelinux_root_is_placeholder (prelinux_root ()))
    {
      size_t l = strlen (prelinux_root ());
      if (l + 1 <= outlen)
	{
	  memcpy (out, prelinux_root (), l + 1);
	  return 0;
	}
    }
  return -1;
}

int
prelinux_sign_file (const char *path)
{
  unsigned char magic[4];
  FILE *f = fopen (path, "rb");
  if (f == NULL)
    return -1;
  size_t n = fread (magic, 1, 4, f);
  fclose (f);
  if (n < 4 || memcmp (magic, "\177ELF", 4) != 0)
    return 0; /* not an ELF: nothing to do */
  /* 64-bit little-endian only (our targets); the vendored signer says
     so itself on anything else.  */
  return sign_file_atomic (path, 1);
}

/* File-level signature check for relocate's sign-only-when-needed
   logic: returns 1 when PATH carries a valid self-signature.  */
int
prelinux_signed (const char *path)
{
  FILE *f = fopen (path, "rb");
  if (f == NULL)
    return 0;
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  uint8_t *buf = malloc (sz > 0 ? (size_t) sz : 1);
  size_t got = 0;
  if (buf != NULL && sz > 0)
    got = fread (buf, 1, (size_t) sz, f);
  fclose (f);
  if (buf == NULL || got != (size_t) sz)
    {
      free (buf);
      return 0;
    }
  const char *reason = NULL;
  int ok = check_selfsign (buf, (size_t) sz, &reason) != 0;
  free (buf);
  return ok;
}
