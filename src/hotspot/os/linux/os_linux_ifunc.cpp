/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#define _GNU_SOURCE 1
#include <stddef.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
#include <link.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <stdlib.h>

// These offsets are for Debian 12 x86_64:
// gdb -batch /lib64/ld-linux-x86-64.so.2 -ex
static unsigned   l_scope_offset = 0x3b0;
static const char l_scope_offset_expr[] = "p &((struct link_map *)0)->l_scope";
static unsigned   l_relocated_offset = 0x334;
static const char l_relocated_offset_expr[] = "p &((struct link_map *)0)->l_relocated";
static unsigned   rtld_global_ro_dl_x86_cpu_features_offset = 0x70;
static const char rtld_global_ro_dl_x86_cpu_features_offset_expr[] = "p (void *)&_rtld_global_ro._dl_x86_cpu_features - (void *)&_rtld_global_ro";
static unsigned   rtld_global_ro_dl_x86_cpu_features_sizeof = 0x1e0;
static const char rtld_global_ro_dl_x86_cpu_features_sizeof_expr[] = "p sizeof(_rtld_global_ro._dl_x86_cpu_features)";
static unsigned   arch_kind_unknown = 0;
static const char arch_kind_unknown_expr[] = "p (int)arch_kind_unknown";
static unsigned   tunable_t_sizeof = 112;
static const char tunable_t_sizeof_expr[] = "p sizeof(tunable_t)";
static unsigned   l_relocated_bitno = 3;

#ifndef DT_RELRSZ
#define DT_RELRSZ       35              /* Total size of RELR relative relocations */
#endif

#define strcmp strcmp_local
static int strcmp_local(const char *s1, const char *s2) {
  const unsigned char *a = (const unsigned char *) s1;
  const unsigned char *b = (const unsigned char *) s2;
  for (;; ++a, ++b) {
    if (*a != *b)
      return *b > *a ? +1 : -1;
    if (*a == 0)
      return 0;
  }
}

#define memcmp memcmp_local
static int memcmp_local(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = (const unsigned char *) s1;
  const unsigned char *b = (const unsigned char *) s2;
  for (; n-- > 0; ++a, ++b) {
    if (*a != *b)
      return *b > *a ? +1 : -1;
  }
  return 0;
}

#define strchr strchr_local
static char *strchr_local(char *s, int c) {
  for (; *s; ++s)
    if ((uint8_t)*s == (uint8_t)c)
      return s;
  return NULL;
}
static const char *strchr_local(const char *cs, int c) {
  return strchr_local((char *)cs, c);
}

#define strlen strlen_local
__attribute__ ((optimize(0))) // otherwise GCC will replace the function by system strlen() again; also fixed by -fno-builtin
static size_t strlen_local(const char *cs) {
  size_t retval = 0;
  while (*cs++)
    ++retval;
  return retval;
}

#define memset memset_local
static void *memset_local(void *m, int c, size_t n) {
  for (uint8_t *d = (uint8_t *)m; n--; ++d)
    *d = c;
  return m;
}

static void ehdr_verify(const Elf64_Ehdr *ehdr) {
  assert(ehdr->e_ident[EI_MAG0] == ELFMAG0);
  assert(ehdr->e_ident[EI_MAG1] == ELFMAG1);
  assert(ehdr->e_ident[EI_MAG2] == ELFMAG2);
  assert(ehdr->e_ident[EI_MAG3] == ELFMAG3);
  assert(ehdr->e_ident[EI_CLASS] == ELFCLASS64);
  assert(ehdr->e_ident[EI_VERSION] == EV_CURRENT);
  assert(ehdr->e_ident[EI_OSABI] == ELFOSABI_NONE
      || ehdr->e_ident[EI_OSABI] == ELFOSABI_GNU/*STT_GNU_IFUNC*/);
}

size_t file_page_size(int fd) {
  off_t retval = lseek(fd, 0, SEEK_END);
  assert(retval != (off_t)-1);
  retval += PAGE_SIZE - 1;
  retval &= -PAGE_SIZE;
  return retval;
}

static const void *file_mmap(int fd) {
  const void *retval = mmap(NULL, file_page_size(fd), PROT_READ, MAP_PRIVATE, fd, 0);
  assert(retval != MAP_FAILED);
  return retval;
}

static void file_munmap(const void *p, int fd) {
  int err = munmap((void *)p, file_page_size(fd));
  assert(!err);
}

struct symtab_lookup {
  const char *name;
  const void *start, *end;
  unsigned sht;
};
static int symtab_lookup_iterate_phdr(struct dl_phdr_info *info, size_t size, void *data_voidp) {
  struct symtab_lookup *data_p = (struct symtab_lookup *)data_voidp;
  const char *filename = info->dlpi_name;
  if (strcmp(filename, "linux-vdso.so.1") == 0)
    return 0; // unused
  if (!*filename)
    filename = "/proc/self/exe";
  int elf_fd = open(filename, O_RDONLY);
  assert(elf_fd != -1);
  const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file_mmap(elf_fd);
  ehdr_verify(ehdr);
  assert(ehdr->e_phentsize == sizeof(Elf64_Phdr));
  assert(ehdr->e_phnum == info->dlpi_phnum);
  const Elf64_Shdr *shdr_base = (const Elf64_Shdr *)(((const uint8_t *)ehdr) + ehdr->e_shoff);
  assert(ehdr->e_shentsize == sizeof(*shdr_base));
  const Elf64_Sym *symtab = NULL;
  size_t sym_count = 0;
  const char *strtab;
  size_t strtab_size;
  for (size_t shdr_ix = 0; shdr_ix < ehdr->e_shnum; ++shdr_ix) {
    const Elf64_Shdr *shdr = shdr_base + shdr_ix;
    //   [34] .symtab           SYMTAB          0000000000000000 0cfb68 003fd8 18     35 642  8
    if (shdr->sh_type == data_p->sht) {
      symtab = (const Elf64_Sym *)(((const uint8_t *)ehdr) + shdr->sh_offset);
      sym_count = shdr->sh_size / sizeof(*symtab);
      assert(shdr->sh_size == sym_count * sizeof(*symtab));
      assert(shdr->sh_link);
      const Elf64_Shdr *strtab_shdr = shdr_base + shdr->sh_link;
      strtab = (const char *)(((const uint8_t *)ehdr) + strtab_shdr->sh_offset);
      strtab_size = strtab_shdr->sh_size;
      break;
    }
  }
  for (size_t sym_ix = 0; sym_ix < sym_count; ++sym_ix) {
    const Elf64_Sym *sym = symtab + sym_ix;
    assert(sym->st_name < strtab_size);
    if (strcmp(strtab + sym->st_name, data_p->name) == 0) {
      //assert(ELF64_ST_BIND(sym->st_info) == STB_LOCAL); // FIXME
      //assert(ELF64_ST_TYPE(sym->st_info) == STT_FUNC); // FIXME
      assert(ELF64_ST_VISIBILITY(sym->st_other) == STV_DEFAULT); // FIXME
      if (sym->st_shndx == SHN_UNDEF) {
        assert(sym->st_value == 0);
        assert(sym->st_size == 0);
        continue;
      }
      assert(sym->st_value != 0);
      assert(sym->st_size != 0);
      // FIXME: We may have found the symbol multiple times - which one is preferred?
      data_p->start = (const void *)(((uintptr_t)sym->st_value) + info->dlpi_addr);
      data_p->end = (const void *)(((const uint8_t *)data_p->start) + sym->st_size);
      break;
    }
  }
  file_munmap(ehdr, elf_fd);
  int err = close(elf_fd);
  assert(!err);
  return 0;
}

static const void *symtab_lookup(const char *name, const void **end_return, unsigned sht) {
  assert(sht == SHT_DYNSYM || sht == SHT_SYMTAB);
  struct symtab_lookup data;
  data.name = name;
  data.start = NULL;
  data.sht= sht;
  int i = dl_iterate_phdr(symtab_lookup_iterate_phdr, &data);
  assert(!i);
  if (sht == SHT_DYNSYM) {
    if (!data.start) {
      fprintf(stderr, "symtab_lookup failed: %s\n", name);
      assert(0);
    }
    const void *dl = dlsym(RTLD_DEFAULT, name);
    assert(dl == data.start);
  }
  if (end_return)
    *end_return = data.end;
  return data.start;
}

static char *file_read(const char *fn) {
  int fd = open(fn, O_RDONLY);
  assert(fd != -1);
  // realloc() calls memmove().
  size_t buf_size = 0x100000;
  char *buf = (char *)malloc(buf_size);
  assert(buf);
  size_t buf_have = 0;
  for (;;) {
    assert(buf_have < buf_size);
    ssize_t got = read(fd, buf + buf_have, buf_size - buf_have);
    assert(got != -1);
    if (got == 0)
      break;
    assert(got > 0);
    assert(buf_have + got <= buf_size);
    buf_have += got;
  }
  assert(buf_have < buf_size);
  buf[buf_have] = 0;
  assert(strlen(buf) == buf_have);
  return buf;
}

static uint64_t read_hex(const char **cs_p) {
  uint64_t retval = 0;
  for (;;)
#define cs (*cs_p)
    switch (*cs) {
    case '0' ... '9':
      retval <<= 4;
      retval |= *cs++ - '0';
      continue;
    case 'a' ... 'f':
      retval <<= 4;
      retval |= *cs++ - 'a' + 0xa;
      continue;
    default:
      return retval;
      break;
    }
#undef cs
}

static int mprotect_read(const void *addr, const void **addr_end_return) {
  uint64_t addr_u = (uintptr_t)addr;
  char *file = file_read("/proc/self/maps");
  int retval = -1;
  for (const char *cs = file; *cs;) {
    // sscanf() calls rawmemchr().
    uint64_t start = read_hex(&cs);
    assert(*cs == '-');
    ++cs;
    uint64_t end = read_hex(&cs);
    assert(*cs == ' ');
    ++cs;
    assert(start < end);
    int rwxp = 0;
    assert(*cs == 'r' || *cs == '-');
    if (*cs++ == 'r')
      rwxp |= 04;
    assert(*cs == 'w' || *cs == '-');
    if (*cs++ == 'w')
      rwxp |= 02;
    assert(*cs == 'x' || *cs == '-');
    if (*cs++ == 'x')
      rwxp |= 01;
    assert(*cs == 's' || *cs == 'p');
    ++cs;
    assert(*cs == ' ');
    ++cs;
    if (start <= addr_u && addr_u < end) {
      if (addr_end_return)
        *addr_end_return = (const void *)(uintptr_t)end;
      retval = rwxp;
      break;
    }
    cs = strchr(cs, '\n');
    assert(cs);
    ++cs;
  }
  if (retval == -1) {
    fprintf(stderr, "Not found an address: %p\n", addr);
    assert(0);
  }
  free(file);
  return retval;
}

static void verify_rwxp(const void *start, const void *end, int rwxp_want) {
  assert((((uintptr_t)start) & (PAGE_SIZE - 1)) == 0);
  assert((((uintptr_t)end  ) & (PAGE_SIZE - 1)) == 0);
  assert(start < end);
  while (start < end) {
    int rwxp_found = mprotect_read(start, &start);
    if (rwxp_found != rwxp_want) { printf("sudo gdb -p %d\n",getpid()); pause(); }
    assert(rwxp_found == rwxp_want);
  }
}

const struct link_map *phdr_info_to_link_map(struct dl_phdr_info *phdr_info) {
  Dl_info info;
  const struct link_map *link_map = NULL;
  int err = dladdr1(phdr_info->dlpi_phdr, &info, (void **)&link_map, RTLD_DL_LINKMAP);
  assert(err == 1);
  assert(link_map);
  return link_map;
}

static void page_align(const void **start_p, const void **end_p) {
  *start_p = (const void *)(((uintptr_t)*start_p) & -PAGE_SIZE);
  assert(*start_p);
  *end_p = (const void *)((((uintptr_t)*end_p) + PAGE_SIZE - 1) & -PAGE_SIZE);
}

static void readonly_unset(const void *start, const void *end) {
  assert((((uintptr_t)start) & (PAGE_SIZE - 1)) == 0);
  assert((((uintptr_t)end  ) & (PAGE_SIZE - 1)) == 0);
  assert(start <= end);
  if (start == end)
    return;
  verify_rwxp(start, end, 04/*r--*/);
  int err = mprotect((void *)start, (const uint8_t *)end - (const uint8_t *)start, PROT_READ | PROT_WRITE);
  assert(!err);
  verify_rwxp(start, end, 06/*rw-*/);
}

static void readonly_reset(const void *start, const void *end) {
  assert((((uintptr_t)start) & (PAGE_SIZE - 1)) == 0);
  assert((((uintptr_t)end  ) & (PAGE_SIZE - 1)) == 0);
  assert(start <= end);
  if (start == end)
    return;
  verify_rwxp(start, end, 06/*rw-*/);
  int err = mprotect((void *)start, (const uint8_t *)end - (const uint8_t *)start, PROT_READ);
  assert(!err);
  verify_rwxp(start, end, 04/*r--*/);
}

static const void *dl_relocate_object_get() {
  const uint8_t *dl_get_tls_static_info = (const uint8_t *)symtab_lookup("_dl_get_tls_static_info", NULL, SHT_DYNSYM);
  const uint8_t and_1shl27_edx_mov_rsi_offset_rbp[] = { 0x81, 0xe2, 0x00, 0x00, 0x00, 0x08, 0x48, 0x89, 0xb5 };
  const uint8_t *p = dl_get_tls_static_info - 1;
  for (;;) {
    if (memcmp(p, and_1shl27_edx_mov_rsi_offset_rbp, sizeof(and_1shl27_edx_mov_rsi_offset_rbp)) == 0)
      break;
    --p;
  }
  const uint8_t push_rbp_mov_rsp_rbp[] = { 0x55, 0x48, 0x89, 0xe5 };
  for (;;) {
    if (memcmp(p, push_rbp_mov_rsp_rbp, sizeof(push_rbp_mov_rsp_rbp)) == 0)
      break;
    --p;
  }
  const uint8_t endbr64[] = { 0xf3, 0x0f, 0x1e, 0xfa };
  if (memcmp(p - sizeof(endbr64), endbr64, sizeof(endbr64)) == 0)
    p -= sizeof(endbr64);
  return p;
}

static void swap(const void **a_p, const void **b_p) {
  const void *p = *a_p;
  *a_p = *b_p;
  *b_p = p;
}

static void intersect(const void **first_start_p, const void **first_end_p, const void **second_start_p, const void **second_end_p) {
  assert((((uintptr_t)*first_start_p) & (PAGE_SIZE - 1)) == 0);
  assert((((uintptr_t)*first_end_p  ) & (PAGE_SIZE - 1)) == 0);
  assert(*first_start_p <= *first_end_p);
  assert((((uintptr_t)*second_start_p) & (PAGE_SIZE - 1)) == 0);
  assert((((uintptr_t)*second_end_p  ) & (PAGE_SIZE - 1)) == 0);
  assert(*second_start_p <= *second_end_p);
  if (*first_start_p > *second_start_p) {
    swap(first_start_p, second_start_p);
    swap(second_start_p, second_start_p);
  }
  if (*second_start_p < *first_end_p) {
    *second_start_p = *first_end_p;
    if (*second_start_p > *second_end_p)
      *second_end_p = *second_start_p;
  }
}

typedef void (*dl_relocate_object_p) (struct link_map *l, const void */*struct r_scope_elem *scope[]*/, int reloc_mode, int consider_profiling);

static int reset_ifunc_iterate_phdr(struct dl_phdr_info *info, size_t size, void *data_unused) {
  dl_relocate_object_p dl_relocate_object = (dl_relocate_object_p)dl_relocate_object_get();
  dl_relocate_object_p dl_relocate_object_symtab = (dl_relocate_object_p)symtab_lookup("_dl_relocate_object", NULL, SHT_SYMTAB);
  assert(dl_relocate_object_symtab == NULL || dl_relocate_object_symtab == dl_relocate_object);
  if (strcmp(info->dlpi_name, "/lib64/ld-linux-x86-64.so.2") == 0) // _dl_relocate_object would crash on scope == NULL.
    return 0; // unused
  const void *relro = NULL;
  const void *relro_end;
  assert(size >= offsetof(struct dl_phdr_info, dlpi_adds));
  for (size_t phdr_ix = 0; phdr_ix < info->dlpi_phnum; ++phdr_ix) {
    const Elf64_Phdr *phdr = info->dlpi_phdr + phdr_ix;
    if (phdr->p_type == PT_GNU_RELRO) {
      // It does not apply: assert(phdr->p_offset == phdr->p_vaddr);
      assert(phdr->p_paddr == phdr->p_vaddr);
      // /lib64/libz.so.1: p_filesz=0x538 > p_memsz=0x550
      assert(!relro);
      relro = (const void *)(uintptr_t)(phdr->p_vaddr + info->dlpi_addr);
      relro_end = (const void *)(((const uint8_t *)relro) + phdr->p_memsz);
      page_align(&relro, &relro_end);
      assert(relro);
    }
  }
  if (relro)
    readonly_unset(relro, relro_end);
  const struct link_map *map = phdr_info_to_link_map(info);
  Elf64_Dyn *dynamic = map->l_ld;
  Elf64_Xword *relxsz_p = NULL;
  Elf64_Xword *relrsz_p = NULL;
  Elf64_Xword *relxcount_p = NULL;
  for (; dynamic->d_tag != DT_NULL; ++dynamic)
    switch (dynamic->d_tag) {
    case DT_RELASZ:
    case DT_RELSZ:
      assert(!relxsz_p);
      relxsz_p = &dynamic->d_un.d_val;
      break;
    case DT_RELRSZ:
      assert(!relrsz_p);
      relrsz_p = &dynamic->d_un.d_val;
      break;
    case DT_RELCOUNT:
    case DT_RELACOUNT:
      assert(!relxcount_p);
      relxcount_p = &dynamic->d_un.d_val;
      break;
    case DT_PLTREL:
      // It is impossible to relocate DT_REL twice.
      assert(dynamic->d_un.d_val == DT_RELA);
      break;
    }
  Elf64_Xword relxsz_saved;
  if (relxsz_p) {
    relxsz_saved = *relxsz_p;
    *relxsz_p = 0;
  }
  Elf64_Xword relrsz_saved;
  if (relrsz_p) {
    relrsz_saved = *relrsz_p;
    *relrsz_p = 0;
  }
  Elf64_Xword relxcount_saved;
  if (relxcount_p) {
    relxcount_saved = *relxcount_p;
    *relxcount_p = 0;
  }
  unsigned *l_relocated_p = (unsigned *)(((const uint8_t *)map) + l_relocated_offset);
  assert(*l_relocated_p & (1 << l_relocated_bitno));
  *l_relocated_p &= ~(1 << l_relocated_bitno);
  assert(!(*l_relocated_p & (1 << l_relocated_bitno)));
  void **l_scope_p = (void **)(((const uint8_t *)map) + l_scope_offset);
  // FIXME: skip ifuncs
  dl_relocate_object((struct link_map *)map, *l_scope_p, 0/*lazy*/, 0/*consider_profiling*/);
  // It was read/write before but dl_relocate_object made it read-only.
  const void *dynamic_start = NULL;
  const void *dynamic_end;
  if (relxsz_p) {
    dynamic_start = relxsz_p;
    dynamic_end = relxsz_p + 1;
  }
  if (relrsz_p) {
    if (!dynamic_start) {
      dynamic_start = relrsz_p;
      dynamic_end = relrsz_p + 1;
    } else {
      if (dynamic_start > relrsz_p)
        dynamic_start = relrsz_p;
      if (dynamic_end < relrsz_p + 1)
        dynamic_end = relrsz_p + 1;
    }
  }
  if (relxcount_p) {
    if (!dynamic_start) {
      dynamic_start = relxcount_p;
      dynamic_end = relxcount_p + 1;
    } else {
      if (dynamic_start > relxcount_p)
        dynamic_start = relxcount_p;
      if (dynamic_end < relxcount_p + 1)
        dynamic_end = relxcount_p + 1;
    }
  }
  if (dynamic_start) {
    page_align(&dynamic_start, &dynamic_end);
    // dl_relocate_object made it already readonly: readonly_reset(relro, relro_end);
    intersect(&relro, &relro_end, &dynamic_start, &dynamic_end);
    readonly_unset(relro, relro_end);
    readonly_unset(dynamic_start, dynamic_end);
  }
  if (relxsz_p)
    *relxsz_p = relxsz_saved;
  if (relrsz_p)
    *relrsz_p = relrsz_saved;
  if (relxcount_p)
    *relxcount_p = relxcount_saved;
  if (dynamic_start)
    readonly_reset(dynamic_start, dynamic_end);
  if (relro)
    readonly_reset(relro, relro_end);
  return 0; // unused
}

/* 00000000000168b0 <__tunable_get_val>:
 * 168b0:       f3 0f 1e fa             endbr64
 * 168b4:       89 ff                   mov    %edi,%edi
 * 168b6:       48 8d 0d e3 f1 01 00    lea    0x1f1e3(%rip),%rcx        # 35aa0 <tunable_list>
 */
static const void *tunable_list_get() {
  const uint8_t *tunable_get_val = (const uint8_t *)symtab_lookup("__tunable_get_val", NULL, SHT_DYNSYM);
  const uint8_t endbr64[] = { 0xf3, 0x0f, 0x1e, 0xfa };
  if (memcmp(tunable_get_val, endbr64, sizeof(endbr64)) == 0)
    tunable_get_val += sizeof(endbr64);
  const uint8_t mov_edi_edi[] = { 0x89, 0xff };
  assert(memcmp(tunable_get_val, mov_edi_edi, sizeof(mov_edi_edi)) == 0);
  tunable_get_val += sizeof(mov_edi_edi);
  const uint8_t lea_offset_rip_rcx[] = { 0x48, 0x8d, 0x0d };
  assert(memcmp(tunable_get_val, lea_offset_rip_rcx, sizeof(lea_offset_rip_rcx)) == 0);
  tunable_get_val += sizeof(lea_offset_rip_rcx);
  return tunable_get_val + 4 + *(const uint32_t *)tunable_get_val;
}

static size_t tunable_list_count() {
  FILE *f = popen("/lib64/ld-linux-x86-64.so.2 --list-tunables", "r");
  assert(f);
  size_t lines = 0;
  for (;;) {
    int i = fgetc(f);
    if (i == EOF)
      break;
    if (i == '\n')
      ++lines;
  }
  assert(!ferror(f));
  assert(feof(f));
  int rc = pclose(f);
  assert(rc == 0);
  return lines;
}

static const void *dl_x86_init_cpu_features_get() {
  const uint8_t *dl_x86_get_cpu_features = (const uint8_t *)symtab_lookup("_dl_x86_get_cpu_features", NULL, SHT_DYNSYM);
  const uint8_t mov_offset_rip_eax[] = { 0x8b, 0x05 };
  const uint8_t *p = dl_x86_get_cpu_features - 1;
  for (;;) {
    if (memcmp(p, mov_offset_rip_eax, sizeof(mov_offset_rip_eax)) == 0)
      break;
    --p;
  }
  const uint8_t endbr64[] = { 0xf3, 0x0f, 0x1e, 0xfa };
  if (memcmp(p - sizeof(endbr64), endbr64, sizeof(endbr64)) == 0)
    p -= sizeof(endbr64);
  return p;
}

typedef void (*dl_x86_init_cpu_features_p)();
static void reset_glibc() {
  const void *rtld_global_ro_end;
  const void *rtld_global_ro = symtab_lookup("_rtld_global_ro", &rtld_global_ro_end, SHT_DYNSYM);
  const void *rtld_global_ro_symtab_end;
  const void *rtld_global_ro_symtab = symtab_lookup("_rtld_global_ro", &rtld_global_ro_symtab_end, SHT_SYMTAB);
  if (rtld_global_ro_symtab) {
    assert(rtld_global_ro_symtab == rtld_global_ro);
    assert(rtld_global_ro_symtab_end == rtld_global_ro_end);
  }
  const void *rtld_global_ro_exact = rtld_global_ro;
  const void *tunable_list = tunable_list_get();
  const void *tunable_list_end = ((const uint8_t *)tunable_list) + tunable_t_sizeof * tunable_list_count();
  const void *tunable_list_symtab_end;
  const void *tunable_list_symtab = symtab_lookup("tunable_list", &tunable_list_symtab_end, SHT_SYMTAB);
  assert(tunable_list_symtab == NULL || tunable_list_symtab == tunable_list);
  assert(tunable_list_symtab == NULL || tunable_list_symtab_end == tunable_list_end);
  page_align(&rtld_global_ro, &rtld_global_ro_end);
  page_align(&tunable_list, &tunable_list_end);
  intersect(&rtld_global_ro, &rtld_global_ro_end, &tunable_list, &tunable_list_end);
  readonly_unset(rtld_global_ro, rtld_global_ro_end);
  readonly_unset(tunable_list, tunable_list_end);
  void *cpu_features = (void *)(((uint8_t *)rtld_global_ro_exact) + rtld_global_ro_dl_x86_cpu_features_offset);
  assert(*(const uint32_t *)cpu_features != arch_kind_unknown); // .basic.kind
  memset(cpu_features, 0, rtld_global_ro_dl_x86_cpu_features_sizeof);
  assert(*(const uint32_t *)cpu_features == arch_kind_unknown); // .basic.kind
  dl_x86_init_cpu_features_p dl_x86_init_cpu_features = (dl_x86_init_cpu_features_p)dl_x86_init_cpu_features_get();
  dl_x86_init_cpu_features_p dl_x86_init_cpu_features_symtab = (dl_x86_init_cpu_features_p)symtab_lookup("_dl_x86_init_cpu_features", NULL, SHT_SYMTAB);
  assert(dl_x86_init_cpu_features_symtab == NULL || dl_x86_init_cpu_features_symtab == dl_x86_init_cpu_features);
  (*dl_x86_init_cpu_features)();
  assert(*(const uint32_t *)cpu_features != arch_kind_unknown); // .basic.kind
  readonly_reset(rtld_global_ro, rtld_global_ro_end);
  readonly_reset(tunable_list, tunable_list_end);
}

static FILE *fetch_offset_popen(const char *expr) {
  char *cmd;
  int err;
  err = asprintf(&cmd, "gdb -batch /lib64/ld-linux-x86-64.so.2 -ex '%s'", expr);
  assert(err > 0);
  FILE *f = popen(cmd, "r");
  assert(f);
  free(cmd);
  return f;
}

static void fetch_offset(unsigned *unsigned_p, const char *expr) {
  FILE *f = fetch_offset_popen(expr);
  char line[LINE_MAX];
  char *s = fgets(line, sizeof(line), f);
  if (s == NULL) {
    assert(feof(f));
    assert(!ferror(f));
    return;
  }
  assert(s == line);
  s = strchr(line, '\n');
  assert(s);
  assert(!s[1]);
  *s = 0;
  s = strrchr(line, ')');
  if (s)
    ++s;
  else {
    s = strrchr(line, '=');
    if (!s) {
      fprintf(stderr, "No ')' or '=' found for: %s\n", expr);
      return;
    }
    ++s;
  }
  int err;
  err = pclose(f);
  char *end;
  unsigned long ul = strtoul(s, &end, 0);
  if (ul >= INT_MAX || (end && *end)) {
    fprintf(stderr,"Number not parseable \"%s\" for: %s\n", s, expr);
    return;
  }
  assert(!err);
  if (*unsigned_p == ul)
    return;
  fprintf(stderr,"%u->%lu %s\n",*unsigned_p,ul,expr);
  *unsigned_p = ul;
}

static void fetch_l_relocated_bitno() {
  FILE *f = fetch_offset_popen("ptype/o struct link_map");
  char line[LINE_MAX];
  for (;;) {
    char *s = fgets(line, sizeof(line), f);
    if (!s) {
      assert(feof(f));
      assert(!ferror(f));
      break;
    }
    s = strchr(line, '\n');
    assert(s);
    assert(!s[1]);
    *s = 0;
    // ptype/o struct link_map
    // /*    820: 3   |       4 */    unsigned int l_relocated : 1;
    //            ^
    // 820==0x334
    if (!strstr(line, " l_relocated : 1;"))
      continue;
    assert(line[0] == '/');
    assert(line[1] == '*');
    char *end;
    unsigned long ul;
    ul = strtoul(line + 2, &end, 0);
    if (!end || *end != ':') {
      fprintf(stderr, "':' not found: %s\n", line);
      break;
    }
    if (ul != l_relocated_offset) {
      fprintf(stderr, "l_relocated_offset = %u != %lu from: %s\n", l_relocated_offset, ul, line);
      break;
    }
    ul = strtoul(end + 1, &end, 0);
    while (end && *end == ' ')
      ++end;
    if (!end || *end != '|') {
      fprintf(stderr, "'|' not found: %s end=%s ul=%lu\n", line, end,ul);
      break;
    }
    if (ul >= 320) {
      fprintf(stderr, "Too large l_relocated_bitno = %lu: %s\n", ul, line);
      break;
    }
    fprintf(stderr,"%u->%lu %s\n",l_relocated_bitno,ul,"l_relocated_bitno");
    l_relocated_bitno = ul;
    break;
  }
  pclose(f);
}

void linux_ifunc_fetch_offsets() {
#define GET(n) fetch_offset(&n, n##_expr)
  GET(l_scope_offset);
  GET(l_relocated_offset);
  GET(rtld_global_ro_dl_x86_cpu_features_offset);
  GET(rtld_global_ro_dl_x86_cpu_features_sizeof);
  GET(arch_kind_unknown);
  GET(tunable_t_sizeof);
  fetch_l_relocated_bitno();
}

void linux_ifunc_reset() {
  // dl_relocate_object() from reset_ifunc_iterate_phdr may be calling glibc ifunc resolvers already.
  reset_glibc();
  int i = dl_iterate_phdr(reset_ifunc_iterate_phdr, NULL/*data*/);
  assert(!i);
}
