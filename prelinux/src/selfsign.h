/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* Vendored from https://github.com/hqzing/ohos-selfsign (0BSD).
   HarmonyOS-compatible ELF self-signing, in-process.  */
#ifndef PRELINUX_SELFSIGN_H
#define PRELINUX_SELFSIGN_H 1

#include <stddef.h>
#include <stdint.h>

/* Sign an ELF file atomically (temp file + rename, mode preserved).
   force=1 strips an existing .codesign section first.  */
int sign_file_atomic (const char *path, int force);

/* Check an in-memory ELF's existing self-signature.  */
int check_selfsign (const uint8_t *elf, size_t elf_len, const char **reason);

/* Strip an existing .codesign section from an in-memory ELF
   (reallocates *buf; caller frees).  Returns 0 on success.  */
int strip_codesign (uint8_t **buf, size_t *buf_len);

#endif
