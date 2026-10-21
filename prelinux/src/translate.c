/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux base SDK — translation module and public API.
   Compiled into both the static `prelinux` binary and libprelinux.so.
   The instance-root storage lives here (one definition per module).  */
#define PRELINUX_STANDALONE 1
#include "prelinux-translate.h"
#include "prelinux.h"

#include <string.h>

const char __prelinux_root_storage[PRELINUX_ROOT_CAPACITY]
  __attribute__ ((visibility ("hidden")))
  = PRELINUX_ROOT_PLACEHOLDER;

const char *
prelinux_root (void)
{
  return __prelinux_root_storage;
}

int
prelinux_translate (const char *path, char *buf, size_t buflen)
{
  const char *r = __prelinux_translate_path (path, buf, buflen);
  if (r == NULL)
    return -1;
  return r == path ? 0 : 1;
}

/* Build the pristine placeholder at runtime.  A compile-time copy of
   the full pattern in this binary would itself be rewritten at install
   time (the installer patches every occurrence), so nothing here may
   ever materialize it as a constant.  */
void
prelinux_pristine_placeholder (char *buf)
{
  memset (buf, 'A', PRELINUX_ROOT_CAPACITY - 1);
  memcpy (buf, "/.prelinux-root-placeholder-", 28);
  buf[PRELINUX_ROOT_CAPACITY - 1] = '\0';
}

int
prelinux_root_is_placeholder (const char *root)
{
  char pristine[PRELINUX_ROOT_CAPACITY];
  prelinux_pristine_placeholder (pristine);
  return strcmp (root, pristine) == 0;
}
