/*
 * Copyright (c) 2006, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2020, Red Hat Inc. All rights reserved.
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

#include "memory/resourceArea.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/vm_version.hpp"

#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/prctl.h>

// IC IVAU trap probe.
// Defined in ic_ivau_probe_linux_aarch64.S.
extern "C" int ic_ivau_probe(void);

#ifndef HWCAP_AES
#define HWCAP_AES   (1<<3)
#endif

#ifndef HWCAP_PMULL
#define HWCAP_PMULL (1<<4)
#endif

#ifndef HWCAP_SHA1
#define HWCAP_SHA1  (1<<5)
#endif

#ifndef HWCAP_SHA2
#define HWCAP_SHA2  (1<<6)
#endif

#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1<<7)
#endif

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1<<8)
#endif

#ifndef HWCAP_DCPOP
#define HWCAP_DCPOP (1<<16)
#endif

#ifndef HWCAP_SHA3
#define HWCAP_SHA3 (1 << 17)
#endif

#ifndef HWCAP_SHA512
#define HWCAP_SHA512 (1 << 21)
#endif

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#ifndef HWCAP_SB
#define HWCAP_SB (1 << 29)
#endif

#ifndef HWCAP_PACA
#define HWCAP_PACA (1 << 30)
#endif

#ifndef HWCAP_FPHP
#define HWCAP_FPHP (1<<9)
#endif

#ifndef HWCAP_ASIMDHP
#define HWCAP_ASIMDHP (1<<10)
#endif

#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1 << 1)
#endif

#ifndef HWCAP2_SVEBITPERM
#define HWCAP2_SVEBITPERM (1 << 4)
#endif

#ifndef HWCAP2_ECV
#define HWCAP2_ECV (1 << 19)
#endif

#ifndef HWCAP2_WFXT
#define HWCAP2_WFXT (1u << 31)
#endif
#ifndef PR_SVE_GET_VL
// For old toolchains which do not have SVE related macros defined.
#define PR_SVE_SET_VL   50
#define PR_SVE_GET_VL   51
#endif

int VM_Version::get_current_sve_vector_length() {
  assert(VM_Version::supports_sve(), "should not call this");
  return prctl(PR_SVE_GET_VL);
}

int VM_Version::set_and_get_current_sve_vector_length(int length) {
  assert(VM_Version::supports_sve(), "should not call this");
  int new_length = prctl(PR_SVE_SET_VL, length);
  return new_length;
}

void VM_Version::update_feature(uint64_t hwcap, Feature_Flag flag, uint64_t hwcap_bitmask) {
  if (hwcap & hwcap_bitmask) {
    _features.set_feature(flag);
  } else {
    _features.clear_feature(flag);
  }
}

void VM_Version::get_os_cpu_info() {

  uint64_t auxv = getauxval(AT_HWCAP);
  uint64_t auxv2 = getauxval(AT_HWCAP2);

  update_feature(auxv,  CPU_FP,         HWCAP_FP         );
  update_feature(auxv,  CPU_ASIMD,      HWCAP_ASIMD      );
  update_feature(auxv,  CPU_EVTSTRM,    HWCAP_EVTSTRM    );
  update_feature(auxv,  CPU_AES,        HWCAP_AES        );
  update_feature(auxv,  CPU_PMULL,      HWCAP_PMULL      );
  update_feature(auxv,  CPU_SHA1,       HWCAP_SHA1       );
  update_feature(auxv,  CPU_SHA2,       HWCAP_SHA2       );
  update_feature(auxv,  CPU_CRC32,      HWCAP_CRC32      );
  update_feature(auxv,  CPU_LSE,        HWCAP_ATOMICS    );
  update_feature(auxv,  CPU_FPHP,       HWCAP_FPHP       );
  update_feature(auxv,  CPU_ASIMDHP,    HWCAP_ASIMDHP    );
  update_feature(auxv,  CPU_DCPOP,      HWCAP_DCPOP      );
  update_feature(auxv,  CPU_SHA3,       HWCAP_SHA3       );
  update_feature(auxv,  CPU_SHA512,     HWCAP_SHA512     );
  update_feature(auxv,  CPU_SVE,        HWCAP_SVE        );
  // CPU_SB is missing but there exists HWCAP_SB
  update_feature(auxv,  CPU_PACA,       HWCAP_PACA       );
  update_feature(auxv2, CPU_SVEBITPERM, HWCAP2_SVEBITPERM);
  update_feature(auxv2, CPU_SVE2,       HWCAP2_SVE2      );
  // CPU_A53MAC is missing as there is no HWCAP*_A53MAC
  update_feature(auxv2, CPU_ECV,        HWCAP2_ECV       );
  update_feature(auxv2, CPU_WFXT,       HWCAP2_WFXT      );
  update_feature(~auxv, CPU_NOTPACA,    HWCAP_PACA       );

  uint64_t ctr_el0;
  uint64_t dczid_el0;
  __asm__ (
    "mrs %0, CTR_EL0\n"
    "mrs %1, DCZID_EL0\n"
    : "=r"(ctr_el0), "=r"(dczid_el0)
  );

  _icache_line_size = (1 << (ctr_el0 & 0x0f)) * 4;
  _dcache_line_size = (1 << ((ctr_el0 >> 16) & 0x0f)) * 4;
  _cache_idc_enabled = ((ctr_el0 >> 28) & 0x1) != 0;
  _cache_dic_enabled = ((ctr_el0 >> 29) & 0x1) != 0;

  // Probe whether IC IVAU is trapped.
  // Must run before VM_Version::initialize() sets NeoverseN1ICacheErratumMitigation.
  _ic_ivau_trapped = (ic_ivau_probe() == 1);

  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }

  if (FILE *f = os::fopen("/proc/cpuinfo", "r")) {
    // need a large buffer as the flags line may include lots of text
    char buf[1024], *p;
    while (fgets(buf, sizeof (buf), f) != nullptr) {
      if ((p = strchr(buf, ':')) != nullptr) {
        long v = strtol(p+1, nullptr, 0);
        if (strncmp(buf, "CPU implementer", sizeof "CPU implementer" - 1) == 0) {
          _cpu = v;
        } else if (strncmp(buf, "CPU variant", sizeof "CPU variant" - 1) == 0) {
          _variant = v;
        } else if (strncmp(buf, "CPU part", sizeof "CPU part" - 1) == 0) {
          if (_model != v)  _model2 = _model;
          _model = v;
        } else if (strncmp(buf, "CPU revision", sizeof "CPU revision" - 1) == 0) {
          _revision = v;
        } else if (strncmp(buf, "flags", sizeof("flags") - 1) == 0) {
          if (strstr(p+1, "dcpop")) {
            guarantee(supports_feature(CPU_DCPOP), "dcpop availability should be consistent");
          }
        }
      }
    }
    fclose(f);
  }
}

void VM_Version::check_os_cpu_info() {
  if (supports_paca() == supports_notpaca()) {
    ResourceMark rm;
    VM_Features paca;
    paca.set_feature(CPU_PACA);
    VM_Features notpaca;
    notpaca.set_feature(CPU_NOTPACA);
    stringStream ss;
    ss.print_cr("For -XX:CPUFeatures, exactly one of the bits PACA (%s) and NOTPACA (%s) must be set.", paca.print_numbers(), notpaca.print_numbers());
    vm_exit_during_initialization(ss.base());
  }
  if (_cpu_features.supports_feature(CPU_LSE) && !supports_lse()) {
    ResourceMark rm;
    VM_Features lse;
    lse.set_feature(VM_Feature_Flag::CPU_LSE);
    stringStream ss;
    // GLIBC_TUNABLES=glibc.cpu.hwcaps is unsupported on aarch64
    ss.print_cr("LSE (%s) cannot be disabled via -XX:CPUFeatures on aarch64.", lse.print_numbers());
    vm_exit_during_initialization(ss.base());
  }
}

static bool read_fully(const char *fname, char *buf, size_t buflen) {
  assert(buf != nullptr, "invalid argument");
  assert(buflen >= 1, "invalid argument");
  int fd = os::open(fname, O_RDONLY, 0);
  if (fd != -1) {
    PRAGMA_DIAG_PUSH
    PRAGMA_NONNULL_IGNORED
    // Suppress false positive gcc warning, which may be an example of
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=87489
    // The warning also hasn't been seen with vanilla gcc release, so may also
    // involve some distro-specific gcc patch.
    ssize_t read_sz = ::read(fd, buf, buflen);
    PRAGMA_DIAG_POP
    ::close(fd);

    // Skip if the contents is just "\n" because some machine only sets
    // '\n' to the board name.
    // (e.g. esys/devices/virtual/dmi/id/board_name)
    if (read_sz > 0 && !(read_sz == 1 && *buf == '\n')) {
      // Replace '\0' to ' '
      for (char *ch = buf; ch < buf + read_sz - 1; ch++) {
        if (*ch == '\0') {
          *ch = ' ';
        }
      }
      buf[read_sz - 1] = '\0';
      return true;
    }
  }
  *buf = '\0';
  return false;
}

void VM_Version::get_compatible_board(char *buf, int buflen) {
  const char *board_name_file_list[] = {
    "/proc/device-tree/compatible",
    "/sys/devices/virtual/dmi/id/board_name",
    "/sys/devices/virtual/dmi/id/product_name",
    nullptr
  };

  for (const char **fname = board_name_file_list; *fname != nullptr; fname++) {
    if (read_fully(*fname, buf, buflen)) {
      return;
    }
  }
}
