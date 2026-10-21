/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 The PreLinux Authors */

/* PreLinux ELF rewriting engine (PT_INTERP / RPATH / RUNPATH).
   Clean-room implementation of the techniques popularized by patchelf
   (GPLv3; not vendored — reimplemented to keep this engine's provenance
   clean):

   - a new-style string that fits the old slot is written in place;
   - otherwise the file gains one appended page holding the new strings
     (and, when no program header slot is free, the relocated program
     header table itself), plus one PT_LOAD segment mapping that page.

   Supported: ELF64, little-endian, ET_EXEC / ET_DYN.  Anything else is
   reported as an error — never a silent partial write.  */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <elf.h>

#define PRELINUX_STANDALONE 1
#include "prelinux-translate.h"
#include "prelinux.h"

#define PAGE 4096

struct prelinux_elf
{
  int fd;
  unsigned char *map;
  size_t size; /* file size at open time */
  Elf64_Ehdr *eh;
  Elf64_Phdr *ph;  /* program headers (follows e_phoff) */
  uint64_t phnum;
  Elf64_Shdr *sh;  /* section headers, NULL when stripped */
  uint64_t shnum;
  char *err;
  size_t errlen;
};

static int
fail (prelinux_elf *e, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (e->err, e->errlen, fmt, ap);
  va_end (ap);
  errno = EINVAL;
  return -1;
}

static int
grow_to (prelinux_elf *e, size_t newsize)
{
  if (ftruncate (e->fd, (off_t) newsize) != 0)
    return fail (e, "ftruncate(%zu): %s", newsize, strerror (errno));
#ifdef __linux__
  void *nm = mremap (e->map, e->size, newsize, MREMAP_MAYMOVE);
  if (nm == MAP_FAILED)
    return fail (e, "mremap: %s", strerror (errno));
  e->map = nm;
#else
  munmap (e->map, e->size);
  void *nm = mmap (0, newsize, PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, 0);
  if (nm == MAP_FAILED)
    return fail (e, "mmap: %s", strerror (errno));
  e->map = nm;
#endif
  e->size = newsize;
  e->eh = (Elf64_Ehdr *) e->map;
  /* Refresh all pointers into the mapping (mremap may have moved).  */
  e->ph = (Elf64_Phdr *) (e->map + e->eh->e_phoff);
  e->sh = e->eh->e_shoff ? (Elf64_Shdr *) (e->map + e->eh->e_shoff) : NULL;
  return 0;
}

/* Re-derive pointers that depend on e_phoff (after a table move).  */
static void
repoint_phdrs (prelinux_elf *e)
{
  e->ph = (Elf64_Phdr *) (e->map + e->eh->e_phoff);
}

prelinux_elf *
prelinux_elf_open (const char *file, char *err, size_t errlen)
{
  prelinux_elf *e = calloc (1, sizeof (*e));
  e->err = err;
  e->errlen = errlen;

  struct stat st;
  if (stat (file, &st) != 0)
    {
      fail (e, "%s: %s", file, strerror (errno));
      goto err;
    }
  if (!S_ISREG (st.st_mode) || st.st_size < (off_t) sizeof (Elf64_Ehdr))
    {
      fail (e, "%s: not a regular file", file);
      goto err;
    }
  e->size = (size_t) st.st_size;
  e->fd = open (file, O_RDWR);
  if (e->fd < 0)
    {
      fail (e, "%s: %s", file, strerror (errno));
      goto err;
    }
  e->map = mmap (0, e->size, PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, 0);
  if (e->map == MAP_FAILED)
    {
      fail (e, "%s: mmap: %s", file, strerror (errno));
      goto err;
    }
  e->eh = (Elf64_Ehdr *) e->map;
  if (memcmp (e->eh->e_ident, ELFMAG, SELFMAG) != 0)
    {
      fail (e, "%s: not an ELF file", file);
      goto err;
    }
  if (e->eh->e_ident[EI_CLASS] != ELFCLASS64
      || e->eh->e_ident[EI_DATA] != ELFDATA2LSB)
    {
      fail (e, "%s: unsupported ELF (only 64-bit little-endian)", file);
      goto err;
    }
  if (e->eh->e_type != ET_EXEC && e->eh->e_type != ET_DYN)
    {
      /* Not an error: relocatable objects & friends are skipped.  */
      e->err[0] = '\0';
      errno = 0;
      goto err;
    }
  if (e->eh->e_phentsize != sizeof (Elf64_Phdr)
      || e->eh->e_phoff + (uint64_t) e->eh->e_phnum * sizeof (Elf64_Phdr)
	   > e->size)
    {
      fail (e, "%s: corrupt program header table", file);
      goto err;
    }

  e->phnum = e->eh->e_phnum;
  e->shnum = e->eh->e_shnum;
  e->sh = NULL;
  if (e->eh->e_shoff != 0)
    {
      if (e->eh->e_shentsize != sizeof (Elf64_Shdr)
	  || e->eh->e_shoff > e->size - sizeof (Elf64_Shdr))
	{
	  fail (e, "%s: corrupt section header table", file);
	  goto err;
	}
      e->sh = (Elf64_Shdr *) (e->map + e->eh->e_shoff);
      if (e->shnum == 0)
	e->shnum = e->sh[0].sh_size;
      if (e->phnum == PN_XNUM)
	e->phnum = e->sh[0].sh_info;
      if (e->eh->e_shoff + e->shnum * sizeof (Elf64_Shdr) > e->size)
	{
	  e->sh = NULL; /* tolerable: sections not needed */
	  e->shnum = 0;
	}
    }
  if (e->eh->e_phoff + e->phnum * sizeof (Elf64_Phdr) > e->size)
    {
      fail (e, "%s: corrupt program header table", file);
      goto err;
    }
  repoint_phdrs (e);
  return e;

 err:
  {
    int saved = errno;
    if (e->map != NULL && e->map != MAP_FAILED)
      munmap (e->map, e->size);
    if (e->fd >= 0)
      close (e->fd);
    free (e);
    errno = saved;
    return NULL;
  }
}

void
prelinux_elf_close (prelinux_elf *e)
{
  if (e == NULL)
    return;
  if (e->map != NULL)
    {
      msync (e->map, e->size, MS_SYNC);
      munmap (e->map, e->size);
    }
  if (e->fd >= 0)
    close (e->fd);
  free (e);
}

int
prelinux_elf_machine (prelinux_elf *e)
{
  return e->eh->e_machine;
}

int
prelinux_elf_has_interp (prelinux_elf *e)
{
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_INTERP)
      return 1;
  return 0;
}

const char *
prelinux_elf_get_interp (prelinux_elf *e)
{
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_INTERP)
      {
	if (e->ph[i].p_offset + e->ph[i].p_filesz > e->size)
	  return NULL;
	return (const char *) e->map + e->ph[i].p_offset;
      }
  return NULL;
}

/* Size of the PT_INTERP segment's file image (0 when none).  relocate
   compares it against the string length: a segment left oversized by a
   previous rewrite (the 254-byte placeholder pass) must be shrunk even
   when the stored path already matches, or the kernel refuses to exec
   the binary (see prelinux_elf_set_interp).  */
size_t
prelinux_elf_interp_size (prelinux_elf *e)
{
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_INTERP)
      return (size_t) e->ph[i].p_filesz;
  return 0;
}

/* Map a virtual address to a file offset through PT_LOAD segments.  */
static int
vaddr_to_off (prelinux_elf *e, uint64_t vaddr, uint64_t *off)
{
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_LOAD && vaddr >= e->ph[i].p_vaddr
	&& vaddr < e->ph[i].p_vaddr + e->ph[i].p_filesz)
      {
	*off = e->ph[i].p_offset + (vaddr - e->ph[i].p_vaddr);
	return 0;
      }
  return -1;
}

/* Append one page holding PAYLOADS (interp string, dynstr image, then
   the moved-and-extended program header table) and one PT_LOAD covering
   it.  Returns the file offset / vaddr of the page.  */
static int
append_page (prelinux_elf *e, const char *new_interp,
	     const unsigned char *new_dynstr, size_t new_dynstr_len,
	     uint64_t *page_off_out, uint64_t *page_vaddr_out,
	     uint64_t *interp_off_out, uint64_t *dynstr_off_out)
{
  size_t interp_len = new_interp ? strlen (new_interp) + 1 : 0;
  uint64_t phdr_bytes = (e->phnum + 1) * sizeof (Elf64_Phdr);

  /* Layout: [interp][dynstr][align 8][phdrs + new PT_LOAD entry].  */
  uint64_t span = ((interp_len + new_dynstr_len + 7) & ~(uint64_t) 7)
		+ phdr_bytes;

  /* Choose the appended page's FILE OFFSET first (past the end of the
     file, so section headers/symtab are never clobbered), then derive
     its vaddr from the image's load bias.  The kernel computes
     AT_PHDR as (first PT_LOAD vaddr - first PT_LOAD offset) + e_phoff,
     i.e. the program header table's file offset is an offset from the
     load bias.  The appended page must therefore live at
     vaddr == load_bias + file_offset: the two coincide for ET_DYN
     (bias 0) but not for ET_EXEC, whose first LOAD sits at e.g.
     0x400000.  A page placed at vaddr == offset in an ET_EXEC puts
     AT_PHDR and the interpreter into unmapped memory: exec succeeds,
     ld.so reads a bogus phdr table and the process dies with SIGSEGV.
     gcc's non-PIE cc1/lto1 exposed this on HarmonyOS PC (2026-10).  */
  uint64_t maxv = 0;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_LOAD)
      {
	uint64_t end = e->ph[i].p_vaddr + e->ph[i].p_memsz;
	if (end > maxv)
	  maxv = end;
      }
  uint64_t load_bias = 0;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_LOAD)
      {
	load_bias = e->ph[i].p_vaddr - e->ph[i].p_offset;
	break;
      }
  uint64_t size_al = (e->size + PAGE - 1) & ~(uint64_t) (PAGE - 1);
  uint64_t page_off = size_al;
  if (maxv > load_bias && maxv - load_bias > page_off)
    page_off = (maxv - load_bias + PAGE - 1) & ~(uint64_t) (PAGE - 1);
  uint64_t page_vaddr = load_bias + page_off;
 retry:
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_LOAD
	&& page_vaddr < e->ph[i].p_vaddr + e->ph[i].p_memsz
	&& e->ph[i].p_vaddr < page_vaddr + span)
      {
	page_off += PAGE;
	page_vaddr = load_bias + page_off;
	goto retry;
      }

  uint64_t cursor = page_off;
  uint64_t interp_off = cursor;
  cursor += interp_len;
  uint64_t dynstr_off = cursor;
  cursor += new_dynstr_len;
  cursor = (cursor + 7) & ~(uint64_t) 7;
  uint64_t new_phoff = cursor;
  cursor += phdr_bytes;

  if (grow_to (e, cursor) != 0)
    return -1;

  if (interp_len)
    memcpy (e->map + interp_off, new_interp, interp_len);
  if (new_dynstr_len)
    memcpy (e->map + dynstr_off, new_dynstr, new_dynstr_len);

  /* Move the program header table into the page, with one extra slot.  */
  Elf64_Phdr *oldph = e->ph;
  Elf64_Phdr *newph = (Elf64_Phdr *) (e->map + new_phoff);
  memcpy (newph, oldph, e->phnum * sizeof (Elf64_Phdr));
  Elf64_Phdr *load = &newph[e->phnum];
  memset (load, 0, sizeof (*load));
  load->p_type = PT_LOAD;
  load->p_flags = PF_R;
  load->p_offset = page_off;
  load->p_vaddr = page_vaddr;
  load->p_paddr = page_vaddr;
  load->p_filesz = span;
  load->p_memsz = span;
  load->p_align = PAGE;

  e->eh->e_phoff = new_phoff;
  e->phnum += 1;
  e->eh->e_phnum = e->phnum < PN_XNUM ? e->phnum : PN_XNUM;
  if (e->eh->e_phnum == PN_XNUM)
    {
      if (e->sh == NULL)
	return fail (e, "phdr overflow needs section header 0");
      e->sh[0].sh_info = e->phnum;
    }
  repoint_phdrs (e);

  /* Keep PT_PHDR (when present) pointing at the live table.  */
  for (uint64_t i = 0; i < e->phnum - 1; i++)
    if (e->ph[i].p_type == PT_PHDR)
      {
	e->ph[i].p_offset = new_phoff;
	e->ph[i].p_vaddr = page_vaddr + (new_phoff - page_off);
	e->ph[i].p_paddr = e->ph[i].p_vaddr;
	e->ph[i].p_filesz = phdr_bytes;
	e->ph[i].p_memsz = phdr_bytes;
      }

  *page_off_out = page_off;
  *page_vaddr_out = page_vaddr;
  *interp_off_out = interp_off;
  *dynstr_off_out = dynstr_off;
  return 0;
}

int
prelinux_elf_set_interp (prelinux_elf *e, const char *newinterp,
			 char *err, size_t errlen)
{
  (void) err;
  (void) errlen;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_INTERP)
      {
	size_t need = strlen (newinterp) + 1;
	if (need <= e->ph[i].p_filesz)
	  {
	    /* Fits the slot: overwrite and NUL-pad the tail.  Shrink the
	       segment to the string actually stored: the HarmonyOS PC
	       kernel copies PT_INTERP into a BINPRM_BUF_SIZE (256-byte)
	       buffer and fails execve with ENOMEM when p_filesz exceeds
	       it, even if the NUL-terminated path inside is shorter.
	       Binaries relocated once against the 254-byte placeholder
	       (the package managers) carry a 292-byte segment; after the
	       import rewrite the path is ~80 bytes, so shrinking is both
	       legal and required (HarmonyOS PC, 2026-10).  */
	    memset (e->map + e->ph[i].p_offset, 0, e->ph[i].p_filesz);
	    memcpy (e->map + e->ph[i].p_offset, newinterp, need);
	    e->ph[i].p_filesz = need;
	    e->ph[i].p_memsz = need;
	    return 0;
	  }
	uint64_t page_off, page_vaddr, interp_off, dynstr_off;
	if (append_page (e, newinterp, NULL, 0, &page_off, &page_vaddr,
			 &interp_off, &dynstr_off) != 0)
	  return -1;
	/* append_page moved the table; find PT_INTERP again.  */
	for (uint64_t j = 0; j < e->phnum; j++)
	  if (e->ph[j].p_type == PT_INTERP)
	    {
	      e->ph[j].p_offset = interp_off;
	      e->ph[j].p_vaddr = page_vaddr + (interp_off - page_off);
	      e->ph[j].p_paddr = e->ph[j].p_vaddr;
	      e->ph[j].p_filesz = need;
	      e->ph[j].p_memsz = need;
	      return 0;
	    }
	return fail (e, "PT_INTERP lost during rewrite");
      }
  return fail (e, "no PT_INTERP segment");
}

/* Repair a legacy ET_EXEC image whose appended interpreter page was
   placed at vaddr == file offset (the pre-2026-10 layout): shift that
   page, and the PHDR/INTERP entries pointing into it, to
   load_bias + offset.  Without this the kernel's AT_PHDR lands in
   unmapped memory and every exec of a non-PIE binary dies with
   SIGSEGV.  Returns 1 when the image was changed, 0 when there is
   nothing to do, -1 on a bogus image.  */
int
prelinux_elf_fix_exec_interp_page (prelinux_elf *e)
{
  if (e->eh->e_type != ET_EXEC)
    return 0;
  uint64_t bias = 0;
  int have_load = 0, interp = -1;
  for (uint64_t i = 0; i < e->phnum; i++)
    {
      if (e->ph[i].p_type == PT_LOAD && !have_load)
	{
	  bias = e->ph[i].p_vaddr - e->ph[i].p_offset;
	  have_load = 1;
	}
      if (e->ph[i].p_type == PT_INTERP)
	interp = (int) i;
    }
  if (!have_load || interp < 0)
    return 0;

  uint64_t iv = e->ph[interp].p_vaddr;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_LOAD && iv >= e->ph[i].p_vaddr
	&& iv < e->ph[i].p_vaddr + e->ph[i].p_memsz)
      {
	uint64_t want = bias + e->ph[i].p_offset;
	if (e->ph[i].p_vaddr == want)
	  return 0; /* already in the fixed layout */
	int64_t delta = (int64_t) (want - e->ph[i].p_vaddr);
	e->ph[i].p_vaddr = want;
	e->ph[i].p_paddr = want;
	for (uint64_t j = 0; j < e->phnum; j++)
	  if (e->ph[j].p_type == PT_PHDR || e->ph[j].p_type == PT_INTERP)
	    {
	      e->ph[j].p_vaddr = (uint64_t) ((int64_t) e->ph[j].p_vaddr
					     + delta);
	      e->ph[j].p_paddr = e->ph[j].p_vaddr;
	    }
	return 1;
      }
  return 0;
}

/* Read the current RPATH/RUNPATH string.  Returns NULL when absent.  */
static char *
get_rpath (prelinux_elf *e, Elf64_Dyn **entry_out, uint64_t *strtab_off_out,
	   uint64_t *strsz_out)
{
  Elf64_Dyn *dyn = NULL;
  uint64_t dynoff = 0, dynfilesz = 0;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_DYNAMIC)
      {
	dynoff = e->ph[i].p_offset;
	dynfilesz = e->ph[i].p_filesz;
	break;
      }
  if (dynoff == 0)
    return NULL;
  if (dynoff + dynfilesz > e->size)
    return NULL;
  dyn = (Elf64_Dyn *) (e->map + dynoff);
  uint64_t ndyn = dynfilesz / sizeof (Elf64_Dyn);

  uint64_t strtab_vaddr = 0, strsz = 0;
  Elf64_Dyn *rpath_entry = NULL;
  for (uint64_t i = 0; i < ndyn; i++)
    {
      if (dyn[i].d_tag == DT_STRTAB)
	strtab_vaddr = dyn[i].d_un.d_ptr;
      else if (dyn[i].d_tag == DT_STRSZ)
	strsz = dyn[i].d_un.d_val;
      else if (dyn[i].d_tag == DT_RPATH || dyn[i].d_tag == DT_RUNPATH)
	rpath_entry = &dyn[i];
    }
  if (rpath_entry == NULL || strtab_vaddr == 0 || strsz == 0)
    return NULL;
  uint64_t strtab_off;
  if (vaddr_to_off (e, strtab_vaddr, &strtab_off) != 0)
    return NULL;
  if (strtab_off + strsz > e->size
      || strtab_off + rpath_entry->d_un.d_val >= e->size)
    return NULL;
  char *s = (char *) e->map + strtab_off + rpath_entry->d_un.d_val;
  if (memchr (s, '\0', strsz - rpath_entry->d_un.d_val) == NULL)
    return NULL;
  *entry_out = rpath_entry;
  *strtab_off_out = strtab_off;
  *strsz_out = strsz;
  return s;
}

/* External hook from relocate.c: translate one RPATH element.
   Returns malloc'd replacement, or NULL to keep the element.  */
extern char *prelinux_relocate_rpath_element (const char *elem);

int
prelinux_elf_rewrite_rpath (prelinux_elf *e, char *err, size_t errlen)
{
  (void) err;
  (void) errlen;
  Elf64_Dyn *entry;
  uint64_t strtab_off, strsz;
  char *cur = get_rpath (e, &entry, &strtab_off, &strsz);
  if (cur == NULL)
    return 0;

  /* Build the replacement value.  */
  size_t cap = strlen (cur) + 4096;
  char *nv = malloc (cap);
  size_t nvlen = 0;
  int changed = 0;
  char *save = NULL;
  for (char *tok = strtok_r (cur, ":", &save); tok;
       tok = strtok_r (NULL, ":", &save))
    {
      char *rep = prelinux_relocate_rpath_element (tok);
      const char *use = rep ? rep : tok;
      size_t l = strlen (use);
      if (nvlen + l + 2 > cap)
	{
	  cap = (cap + l + 2) * 2;
	  nv = realloc (nv, cap);
	}
      memcpy (nv + nvlen, use, l);
      nvlen += l;
      nv[nvlen++] = ':';
      if (rep)
	{
	  free (rep);
	  changed = 1;
	}
    }
  if (nvlen)
    nvlen--; /* drop trailing ':' */
  nv[nvlen] = '\0';
  if (!changed || strcmp (nv, cur) == 0)
    {
      free (nv);
      return 0;
    }

  uint64_t old_off = strtab_off + entry->d_un.d_val;
  size_t old_len = strlen (cur) + 1;
  if (nvlen + 1 <= old_len)
    {
      memset (e->map + old_off, 0, old_len);
      memcpy (e->map + old_off, nv, nvlen + 1);
      free (nv);
      return 1;
    }

  /* Rebuild .dynstr: old bytes + new string appended, in a new page.  */
  size_t newlen = strsz + nvlen + 1;
  unsigned char *newstr = malloc (newlen);
  memcpy (newstr, e->map + strtab_off, strsz);
  memcpy (newstr + strsz, nv, nvlen + 1);
  free (nv);

  uint64_t page_off, page_vaddr, interp_off, dynstr_off;
  if (append_page (e, NULL, newstr, newlen, &page_off, &page_vaddr,
		   &interp_off, &dynstr_off) != 0)
    {
      free (newstr);
      return -1;
    }
  free (newstr);

  /* The dynamic table itself may have moved with the phdr table — the
     .dynamic SECTION did not move; find PT_DYNAMIC again.  */
  uint64_t dynoff = 0, dynfilesz = 0;
  for (uint64_t i = 0; i < e->phnum; i++)
    if (e->ph[i].p_type == PT_DYNAMIC)
      {
	dynoff = e->ph[i].p_offset;
	dynfilesz = e->ph[i].p_filesz;
	break;
      }
  Elf64_Dyn *dyn = (Elf64_Dyn *) (e->map + dynoff);
  uint64_t ndyn = dynfilesz / sizeof (Elf64_Dyn);
  uint64_t strtab_vaddr = page_vaddr + (dynstr_off - page_off);
  for (uint64_t i = 0; i < ndyn; i++)
    {
      if (dyn[i].d_tag == DT_STRTAB)
	dyn[i].d_un.d_ptr = strtab_vaddr;
      else if (dyn[i].d_tag == DT_STRSZ)
	dyn[i].d_un.d_val = newlen;
      else if (dyn[i].d_tag == DT_RPATH || dyn[i].d_tag == DT_RUNPATH)
	dyn[i].d_un.d_val = strsz; /* the appended string's offset */
    }

  /* Keep the .dynstr section header in sync when sections exist.  */
  if (e->sh != NULL)
    for (uint64_t i = 0; i < e->shnum; i++)
      if (e->sh[i].sh_offset == strtab_off
	  && e->sh[i].sh_type == SHT_STRTAB)
	{
	  e->sh[i].sh_offset = dynstr_off;
	  e->sh[i].sh_addr = strtab_vaddr;
	  e->sh[i].sh_size = newlen;
	  break;
	}
  return 1;
}

/* -------- instance-root slot rewriting (binary, in place) ----------- */

/* Verify that the bytes [p, p+PRELINUX_ROOT_CAPACITY) are exactly
   CONTENT followed by NUL padding.  */
static int
slot_matches (const unsigned char *p, size_t avail, const char *content,
	      size_t clen)
{
  if (avail < PRELINUX_ROOT_CAPACITY)
    return 0;
  if (memcmp (p, content, clen) != 0)
    return 0;
  for (size_t i = clen; i < PRELINUX_ROOT_CAPACITY; i++)
    if (p[i] != 0)
      return 0;
  return 1;
}

long
prelinux_rewrite_root_slots (const char *file, const char *oldroot,
			     const char *newroot)
{
  /* The search keys: the pristine placeholder, or OLDROOT.  Both occupy
     a NUL-padded 512-byte slot.  The pristine pattern is built at
     runtime so that this binary carries no constant copy of it (the
     installer rewrites every constant occurrence).  */
  char pristine[PRELINUX_ROOT_CAPACITY];
  prelinux_pristine_placeholder (pristine);
  const char *keys[2];
  size_t keylens[2];
  int nkeys = 0;
  keys[nkeys] = pristine;
  keylens[nkeys++] = PRELINUX_ROOT_CAPACITY - 1;
  if (oldroot != NULL)
    {
      keys[nkeys] = oldroot;
      keylens[nkeys++] = strlen (oldroot);
    }

  size_t newlen = strlen (newroot);
  if (newlen + 1 > PRELINUX_ROOT_CAPACITY)
    {
      errno = ENAMETOOLONG;
      return -1;
    }

  int fd = open (file, O_RDWR);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
    {
      close (fd);
      errno = EINVAL;
      return -1;
    }
  size_t size = (size_t) st.st_size;
  if (size < PRELINUX_ROOT_CAPACITY)
    {
      close (fd);
      return 0;
    }
  unsigned char *map = mmap (0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			     fd, 0);
  if (map == MAP_FAILED)
    {
      close (fd);
      return -1;
    }

  long count = 0;
  for (size_t o = 0; o + PRELINUX_ROOT_CAPACITY <= size; o++)
    {
      if (map[o] != '/') /* both keys are absolute paths */
	continue;
      for (int k = 0; k < nkeys; k++)
	if (slot_matches (map + o, size - o, keys[k], keylens[k]))
	  {
	    memset (map + o, 0, PRELINUX_ROOT_CAPACITY);
	    memcpy (map + o, newroot, newlen);
	    count++;
	    o += PRELINUX_ROOT_CAPACITY - 1;
	    break;
	  }
    }

  if (count)
    msync (map, size, MS_SYNC);
  munmap (map, size);
  close (fd);
  return count;
}
