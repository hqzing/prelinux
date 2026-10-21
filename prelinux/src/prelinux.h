/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux base SDK — internal shared declarations. */
#ifndef BASE_SRC_PRELINUX_H
#define BASE_SRC_PRELINUX_H 1

#include <stddef.h>

/* Longest PT_INTERP path the host kernel can execute.  The kernel
   copies the interpreter path into a BINPRM_BUF_SIZE (256-byte) buffer;
   HarmonyOS PC returns ENOMEM from execve once the path is 255 bytes or
   longer, and Linux behaves comparably.  Same constant bounds the
   instance-root length: root + "/glibc/lib/<ld.so>" must fit.
   (Before 2026-10 this was discovered the hard way: package managers
   relocated once against the 254-byte placeholder carried a 292-byte
   PT_INTERP segment and could not be executed on a device.)  */
#define PRELINUX_INTERP_MAX 254

/* Public API, also exported by libprelinux.so.  */
const char *prelinux_root (void);
int prelinux_translate (const char *path, char *buf, size_t buflen);

/* Nonzero if ROOT is the pristine (never installed) placeholder.  */
int prelinux_root_is_placeholder (const char *root);

/* Build the pristine placeholder at runtime (never a compile-time
   constant in tool binaries: installers rewrite every occurrence).  */
void prelinux_pristine_placeholder (char *buf);

/* Resolve the instance root for CLI operations, in priority order
   EXPLICIT (--root) > $PRELINUX_ROOT > probing our own
   executable path (…/prelinux/bin/prelinux → the instance root two levels
   up) > this binary's compiled-in storage (error if pristine).
   Returns 0 and fills OUT on success.  */
int prelinux_resolve_root (const char *explicit_root, char *out,
			   size_t outlen);

/* Sign an ELF file with the vendored ohos-selfsign implementation
   (in-process).  Returns 0 on success, nonzero on failure.  */
int prelinux_sign_file (const char *path);

/* Returns 1 when PATH is an ELF carrying a valid self-signature.  */
int prelinux_signed (const char *path);

/* relocate.c */
int prelinux_cmd_relocate (int argc, char **argv);

/* overlay.c — replace the distro's glibc runtime files with ours.
   LABEL prefixes messages, SIGN signs each replaced file, QUIET
   suppresses the per-file lines.  Returns the number of files
   overlaid; exits on a version mismatch or I/O error.  */
int prelinux_overlay_glibc (const char *root, const char *label, int sign,
			    int quiet);

/* Put our static ldconfig back over the distro's copies (a libc-bin
   upgrade restores a distro ldconfig that would bypass translation).
   Returns the number of files replaced.  */
int prelinux_restore_ldconfig (const char *root, int quiet);

/* Distro glibc version seen by the last overlay ("2.39" style), or
   NULL.  */
const char *prelinux_overlay_distro_glibc (void);

/* import.c */
int prelinux_cmd_import (int argc, char **argv);

/* elf.c — ELF rewriting engine */
struct prelinux_elf;
typedef struct prelinux_elf prelinux_elf;

/* Open FILE as an ELF object; returns NULL (not an ELF) sets *err to 0,
   or NULL with *err set on real errors.  */
prelinux_elf *prelinux_elf_open (const char *file, char *err, size_t errlen);
void prelinux_elf_close (prelinux_elf *e);

/* ELF machine (ELF header e_machine), or -1 when unknown.  */
int prelinux_elf_machine (prelinux_elf *e);

/* Nonzero if the object has a PT_INTERP (i.e. is a dynamic executable).  */
int prelinux_elf_has_interp (prelinux_elf *e);

/* Rewrite PT_INTERP to NEWINTERP, appending a PT_LOAD page when the new
   interpreter does not fit into the old slot (patchelf-equivalent
   behavior).  Returns 0 on success, -1 with a message in ERR on
   failure; the file is never left half-written.  */
int prelinux_elf_set_interp (prelinux_elf *e, const char *newinterp,
			     char *err, size_t errlen);

/* Rewrite DT_RPATH/DT_RUNPATH: every absolute entry is passed through
   the path translation with ROOT.  Returns 0 (unchanged or rewritten),
   1 when changed, -1 on error.  */
int prelinux_elf_rewrite_rpath (prelinux_elf *e, char *err, size_t errlen);

/* Current interpreter string (NULL when none).  */
const char *prelinux_elf_get_interp (prelinux_elf *e);

/* Size of the PT_INTERP segment's file image (0 when none); used to
   shrink oversized segments left over from the placeholder rewrite.  */
size_t prelinux_elf_interp_size (prelinux_elf *e);

/* Repair an ET_EXEC image whose appended interpreter page sits at
   vaddr == file offset (legacy layout): shift it to load_bias + offset
   so the kernel's AT_PHDR stays valid.  Returns 1 when changed, 0 when
   nothing to do, -1 on a bogus image.  */
int prelinux_elf_fix_exec_interp_page (prelinux_elf *e);

/* Scan a file for the 255-byte instance-root slot(s) holding OLDROOT
   (or the pristine placeholder when OLDROOT is NULL) and rewrite them
   to NEWROOT (NUL padded).  Returns number of slots rewritten, or -1.  */
long prelinux_rewrite_root_slots (const char *file, const char *oldroot,
				  const char *newroot);

#endif
