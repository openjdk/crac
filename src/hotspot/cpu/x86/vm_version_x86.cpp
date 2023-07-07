/*
 * Copyright (c) 1997, 2021, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "jvm.h"
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/codeBlob.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/virtualizationSupport.hpp"
#if INCLUDE_CPU_FEATURE_ACTIVE
# include <sys/platform/x86.h>
#endif
#if INCLUDE_LD_SO_LIST_DIAGNOSTICS
# include <link.h>
# include <sys/wait.h>
#endif

#include OS_HEADER_INLINE(os)

int VM_Version::_cpu;
int VM_Version::_model;
int VM_Version::_stepping;
bool VM_Version::_has_intel_jcc_erratum;
VM_Version::CpuidInfo VM_Version::_cpuid_info = { 0, };
uint64_t VM_Version::_glibc_features;

#define DECLARE_CPU_FEATURE_NAME(id, name, bit) name,
const char* VM_Version::      _features_names[] = {   CPU_FEATURE_FLAGS(DECLARE_CPU_FEATURE_NAME)};
const char* VM_Version::_glibc_features_names[] = { GLIBC_FEATURE_FLAGS(DECLARE_CPU_FEATURE_NAME)};
#undef DECLARE_CPU_FEATURE_FLAG

// Address of instruction which causes SEGV
address VM_Version::_cpuinfo_segv_addr = 0;
// Address of instruction after the one which causes SEGV
address VM_Version::_cpuinfo_cont_addr = 0;

static BufferBlob* stub_blob;
static const int stub_size = 2000;

extern "C" {
  typedef void (*get_cpu_info_stub_t)(void*);
  typedef void (*detect_virt_stub_t)(uint32_t, uint32_t*);
}
static get_cpu_info_stub_t get_cpu_info_stub = NULL;
static detect_virt_stub_t detect_virt_stub = NULL;


class VM_Version_StubGenerator: public StubCodeGenerator {
 public:

  VM_Version_StubGenerator(CodeBuffer *c) : StubCodeGenerator(c) {}

  address generate_get_cpu_info() {
    // Flags to test CPU type.
    const uint32_t HS_EFL_AC = 0x40000;
    const uint32_t HS_EFL_ID = 0x200000;
    // Values for when we don't have a CPUID instruction.
    const int      CPU_FAMILY_SHIFT = 8;
    const uint32_t CPU_FAMILY_386 = (3 << CPU_FAMILY_SHIFT);
    const uint32_t CPU_FAMILY_486 = (4 << CPU_FAMILY_SHIFT);
    bool use_evex = FLAG_IS_DEFAULT(UseAVX) || (UseAVX > 2);

    Label detect_486, cpu486, detect_586, std_cpuid1, std_cpuid4;
    Label sef_cpuid, ext_cpuid, ext_cpuid1, ext_cpuid5, ext_cpuid7, ext_cpuid8, done, wrapup;
    Label legacy_setup, save_restore_except, legacy_save_restore, start_simd_check;

    StubCodeMark mark(this, "VM_Version", "get_cpu_info_stub");
#   define __ _masm->

    address start = __ pc();

    //
    // void get_cpu_info(VM_Version::CpuidInfo* cpuid_info);
    //
    // LP64: rcx and rdx are first and second argument registers on windows

    __ push(rbp);
#ifdef _LP64
    __ mov(rbp, c_rarg0); // cpuid_info address
#else
    __ movptr(rbp, Address(rsp, 8)); // cpuid_info address
#endif
    __ push(rbx);
    __ push(rsi);
    __ pushf();          // preserve rbx, and flags
    __ pop(rax);
    __ push(rax);
    __ mov(rcx, rax);
    //
    // if we are unable to change the AC flag, we have a 386
    //
    __ xorl(rax, HS_EFL_AC);
    __ push(rax);
    __ popf();
    __ pushf();
    __ pop(rax);
    __ cmpptr(rax, rcx);
    __ jccb(Assembler::notEqual, detect_486);

    __ movl(rax, CPU_FAMILY_386);
    __ movl(Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())), rax);
    __ jmp(done);

    //
    // If we are unable to change the ID flag, we have a 486 which does
    // not support the "cpuid" instruction.
    //
    __ bind(detect_486);
    __ mov(rax, rcx);
    __ xorl(rax, HS_EFL_ID);
    __ push(rax);
    __ popf();
    __ pushf();
    __ pop(rax);
    __ cmpptr(rcx, rax);
    __ jccb(Assembler::notEqual, detect_586);

    __ bind(cpu486);
    __ movl(rax, CPU_FAMILY_486);
    __ movl(Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())), rax);
    __ jmp(done);

    //
    // At this point, we have a chip which supports the "cpuid" instruction
    //
    __ bind(detect_586);
    __ xorl(rax, rax);
    __ cpuid();
    __ orl(rax, rax);
    __ jcc(Assembler::equal, cpu486);   // if cpuid doesn't support an input
                                        // value of at least 1, we give up and
                                        // assume a 486
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid0_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    __ cmpl(rax, 0xa);                  // Is cpuid(0xB) supported?
    __ jccb(Assembler::belowEqual, std_cpuid4);

    //
    // cpuid(0xB) Processor Topology
    //
    __ movl(rax, 0xb);
    __ xorl(rcx, rcx);   // Threads level
    __ cpuid();

    __ lea(rsi, Address(rbp, in_bytes(VM_Version::tpl_cpuidB0_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    __ movl(rax, 0xb);
    __ movl(rcx, 1);     // Cores level
    __ cpuid();
    __ push(rax);
    __ andl(rax, 0x1f);  // Determine if valid topology level
    __ orl(rax, rbx);    // eax[4:0] | ebx[0:15] == 0 indicates invalid level
    __ andl(rax, 0xffff);
    __ pop(rax);
    __ jccb(Assembler::equal, std_cpuid4);

    __ lea(rsi, Address(rbp, in_bytes(VM_Version::tpl_cpuidB1_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    __ movl(rax, 0xb);
    __ movl(rcx, 2);     // Packages level
    __ cpuid();
    __ push(rax);
    __ andl(rax, 0x1f);  // Determine if valid topology level
    __ orl(rax, rbx);    // eax[4:0] | ebx[0:15] == 0 indicates invalid level
    __ andl(rax, 0xffff);
    __ pop(rax);
    __ jccb(Assembler::equal, std_cpuid4);

    __ lea(rsi, Address(rbp, in_bytes(VM_Version::tpl_cpuidB2_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // cpuid(0x4) Deterministic cache params
    //
    __ bind(std_cpuid4);
    __ movl(rax, 4);
    __ cmpl(rax, Address(rbp, in_bytes(VM_Version::std_cpuid0_offset()))); // Is cpuid(0x4) supported?
    __ jccb(Assembler::greater, std_cpuid1);

    __ xorl(rcx, rcx);   // L1 cache
    __ cpuid();
    __ push(rax);
    __ andl(rax, 0x1f);  // Determine if valid cache parameters used
    __ orl(rax, rax);    // eax[4:0] == 0 indicates invalid cache
    __ pop(rax);
    __ jccb(Assembler::equal, std_cpuid1);

    __ lea(rsi, Address(rbp, in_bytes(VM_Version::dcp_cpuid4_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Standard cpuid(0x1)
    //
    __ bind(std_cpuid1);
    __ movl(rax, 1);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Check if OS has enabled XGETBV instruction to access XCR0
    // (OSXSAVE feature flag) and CPU supports AVX
    //
    __ andl(rcx, 0x18000000); // cpuid1 bits osxsave | avx
    __ cmpl(rcx, 0x18000000);
    __ jccb(Assembler::notEqual, sef_cpuid); // jump if AVX is not supported

    //
    // XCR0, XFEATURE_ENABLED_MASK register
    //
    __ xorl(rcx, rcx);   // zero for XCR0 register
    __ xgetbv();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::xem_xcr0_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rdx);

    //
    // cpuid(0x7) Structured Extended Features
    //
    __ bind(sef_cpuid);
    __ movl(rax, 7);
    __ cmpl(rax, Address(rbp, in_bytes(VM_Version::std_cpuid0_offset()))); // Is cpuid(0x7) supported?
    __ jccb(Assembler::greater, ext_cpuid);

    __ xorl(rcx, rcx);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::sef_cpuid7_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi, 12), rdx);

    //
    // Extended cpuid(0x80000000)
    //
    __ bind(ext_cpuid);
    __ movl(rax, 0x80000000);
    __ cpuid();
    __ cmpl(rax, 0x80000000);     // Is cpuid(0x80000001) supported?
    __ jcc(Assembler::belowEqual, done);
    __ cmpl(rax, 0x80000004);     // Is cpuid(0x80000005) supported?
    __ jcc(Assembler::belowEqual, ext_cpuid1);
    __ cmpl(rax, 0x80000006);     // Is cpuid(0x80000007) supported?
    __ jccb(Assembler::belowEqual, ext_cpuid5);
    __ cmpl(rax, 0x80000007);     // Is cpuid(0x80000008) supported?
    __ jccb(Assembler::belowEqual, ext_cpuid7);
    __ cmpl(rax, 0x80000008);     // Is cpuid(0x80000009 and above) supported?
    __ jccb(Assembler::belowEqual, ext_cpuid8);
    __ cmpl(rax, 0x8000001E);     // Is cpuid(0x8000001E) supported?
    __ jccb(Assembler::below, ext_cpuid8);
    //
    // Extended cpuid(0x8000001E)
    //
    __ movl(rax, 0x8000001E);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ext_cpuid1E_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Extended cpuid(0x80000008)
    //
    __ bind(ext_cpuid8);
    __ movl(rax, 0x80000008);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ext_cpuid8_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Extended cpuid(0x80000007)
    //
    __ bind(ext_cpuid7);
    __ movl(rax, 0x80000007);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ext_cpuid7_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Extended cpuid(0x80000005)
    //
    __ bind(ext_cpuid5);
    __ movl(rax, 0x80000005);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ext_cpuid5_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Extended cpuid(0x80000001)
    //
    __ bind(ext_cpuid1);
    __ movl(rax, 0x80000001);
    __ cpuid();
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ext_cpuid1_offset())));
    __ movl(Address(rsi, 0), rax);
    __ movl(Address(rsi, 4), rbx);
    __ movl(Address(rsi, 8), rcx);
    __ movl(Address(rsi,12), rdx);

    //
    // Check if OS has enabled XGETBV instruction to access XCR0
    // (OSXSAVE feature flag) and CPU supports AVX
    //
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())));
    __ movl(rcx, 0x18000000); // cpuid1 bits osxsave | avx
    __ andl(rcx, Address(rsi, 8)); // cpuid1 bits osxsave | avx
    __ cmpl(rcx, 0x18000000);
    __ jccb(Assembler::notEqual, done); // jump if AVX is not supported

    __ movl(rax, 0x6);
    __ andl(rax, Address(rbp, in_bytes(VM_Version::xem_xcr0_offset()))); // xcr0 bits sse | ymm
    __ cmpl(rax, 0x6);
    __ jccb(Assembler::equal, start_simd_check); // return if AVX is not supported

    // we need to bridge farther than imm8, so we use this island as a thunk
    __ bind(done);
    __ jmp(wrapup);

    __ bind(start_simd_check);
    //
    // Some OSs have a bug when upper 128/256bits of YMM/ZMM
    // registers are not restored after a signal processing.
    // Generate SEGV here (reference through NULL)
    // and check upper YMM/ZMM bits after it.
    //
    intx saved_useavx = UseAVX;
    intx saved_usesse = UseSSE;

    // If UseAVX is unitialized or is set by the user to include EVEX
    if (use_evex) {
      // check _cpuid_info.sef_cpuid7_ebx.bits.avx512f
      __ lea(rsi, Address(rbp, in_bytes(VM_Version::sef_cpuid7_offset())));
      __ movl(rax, 0x10000);
      __ andl(rax, Address(rsi, 4)); // xcr0 bits sse | ymm
      __ cmpl(rax, 0x10000);
      __ jccb(Assembler::notEqual, legacy_setup); // jump if EVEX is not supported
      // check _cpuid_info.xem_xcr0_eax.bits.opmask
      // check _cpuid_info.xem_xcr0_eax.bits.zmm512
      // check _cpuid_info.xem_xcr0_eax.bits.zmm32
      __ movl(rax, 0xE0);
      __ andl(rax, Address(rbp, in_bytes(VM_Version::xem_xcr0_offset()))); // xcr0 bits sse | ymm
      __ cmpl(rax, 0xE0);
      __ jccb(Assembler::notEqual, legacy_setup); // jump if EVEX is not supported

      if (FLAG_IS_DEFAULT(UseAVX)) {
        __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())));
        __ movl(rax, Address(rsi, 0));
        __ cmpl(rax, 0x50654);              // If it is Skylake
        __ jcc(Assembler::equal, legacy_setup);
      }
      // EVEX setup: run in lowest evex mode
      VM_Version::set_evex_cpuFeatures(); // Enable temporary to pass asserts
      UseAVX = 3;
      UseSSE = 2;
#ifdef _WINDOWS
      // xmm5-xmm15 are not preserved by caller on windows
      // https://msdn.microsoft.com/en-us/library/9z1stfyw.aspx
      __ subptr(rsp, 64);
      __ evmovdqul(Address(rsp, 0), xmm7, Assembler::AVX_512bit);
#ifdef _LP64
      __ subptr(rsp, 64);
      __ evmovdqul(Address(rsp, 0), xmm8, Assembler::AVX_512bit);
      __ subptr(rsp, 64);
      __ evmovdqul(Address(rsp, 0), xmm31, Assembler::AVX_512bit);
#endif // _LP64
#endif // _WINDOWS

      // load value into all 64 bytes of zmm7 register
      __ movl(rcx, VM_Version::ymm_test_value());
      __ movdl(xmm0, rcx);
      __ vpbroadcastd(xmm0, xmm0, Assembler::AVX_512bit);
      __ evmovdqul(xmm7, xmm0, Assembler::AVX_512bit);
#ifdef _LP64
      __ evmovdqul(xmm8, xmm0, Assembler::AVX_512bit);
      __ evmovdqul(xmm31, xmm0, Assembler::AVX_512bit);
#endif
      VM_Version::clean_cpuFeatures();
      __ jmp(save_restore_except);
    }

    __ bind(legacy_setup);
    // AVX setup
    VM_Version::set_avx_cpuFeatures(); // Enable temporary to pass asserts
    UseAVX = 1;
    UseSSE = 2;
#ifdef _WINDOWS
    __ subptr(rsp, 32);
    __ vmovdqu(Address(rsp, 0), xmm7);
#ifdef _LP64
    __ subptr(rsp, 32);
    __ vmovdqu(Address(rsp, 0), xmm8);
    __ subptr(rsp, 32);
    __ vmovdqu(Address(rsp, 0), xmm15);
#endif // _LP64
#endif // _WINDOWS

    // load value into all 32 bytes of ymm7 register
    __ movl(rcx, VM_Version::ymm_test_value());

    __ movdl(xmm0, rcx);
    __ pshufd(xmm0, xmm0, 0x00);
    __ vinsertf128_high(xmm0, xmm0);
    __ vmovdqu(xmm7, xmm0);
#ifdef _LP64
    __ vmovdqu(xmm8, xmm0);
    __ vmovdqu(xmm15, xmm0);
#endif
    VM_Version::clean_cpuFeatures();

    __ bind(save_restore_except);
    __ xorl(rsi, rsi);
    VM_Version::set_cpuinfo_segv_addr(__ pc());
    // Generate SEGV
    __ movl(rax, Address(rsi, 0));

    VM_Version::set_cpuinfo_cont_addr(__ pc());
    // Returns here after signal. Save xmm0 to check it later.

    // If UseAVX is unitialized or is set by the user to include EVEX
    if (use_evex) {
      // check _cpuid_info.sef_cpuid7_ebx.bits.avx512f
      __ lea(rsi, Address(rbp, in_bytes(VM_Version::sef_cpuid7_offset())));
      __ movl(rax, 0x10000);
      __ andl(rax, Address(rsi, 4));
      __ cmpl(rax, 0x10000);
      __ jcc(Assembler::notEqual, legacy_save_restore);
      // check _cpuid_info.xem_xcr0_eax.bits.opmask
      // check _cpuid_info.xem_xcr0_eax.bits.zmm512
      // check _cpuid_info.xem_xcr0_eax.bits.zmm32
      __ movl(rax, 0xE0);
      __ andl(rax, Address(rbp, in_bytes(VM_Version::xem_xcr0_offset()))); // xcr0 bits sse | ymm
      __ cmpl(rax, 0xE0);
      __ jcc(Assembler::notEqual, legacy_save_restore);

      if (FLAG_IS_DEFAULT(UseAVX)) {
        __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())));
        __ movl(rax, Address(rsi, 0));
        __ cmpl(rax, 0x50654);              // If it is Skylake
        __ jcc(Assembler::equal, legacy_save_restore);
      }
      // EVEX check: run in lowest evex mode
      VM_Version::set_evex_cpuFeatures(); // Enable temporary to pass asserts
      UseAVX = 3;
      UseSSE = 2;
      __ lea(rsi, Address(rbp, in_bytes(VM_Version::zmm_save_offset())));
      __ evmovdqul(Address(rsi, 0), xmm0, Assembler::AVX_512bit);
      __ evmovdqul(Address(rsi, 64), xmm7, Assembler::AVX_512bit);
#ifdef _LP64
      __ evmovdqul(Address(rsi, 128), xmm8, Assembler::AVX_512bit);
      __ evmovdqul(Address(rsi, 192), xmm31, Assembler::AVX_512bit);
#endif

#ifdef _WINDOWS
#ifdef _LP64
      __ evmovdqul(xmm31, Address(rsp, 0), Assembler::AVX_512bit);
      __ addptr(rsp, 64);
      __ evmovdqul(xmm8, Address(rsp, 0), Assembler::AVX_512bit);
      __ addptr(rsp, 64);
#endif // _LP64
      __ evmovdqul(xmm7, Address(rsp, 0), Assembler::AVX_512bit);
      __ addptr(rsp, 64);
#endif // _WINDOWS
      generate_vzeroupper(wrapup);
      VM_Version::clean_cpuFeatures();
      UseAVX = saved_useavx;
      UseSSE = saved_usesse;
      __ jmp(wrapup);
   }

    __ bind(legacy_save_restore);
    // AVX check
    VM_Version::set_avx_cpuFeatures(); // Enable temporary to pass asserts
    UseAVX = 1;
    UseSSE = 2;
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::ymm_save_offset())));
    __ vmovdqu(Address(rsi, 0), xmm0);
    __ vmovdqu(Address(rsi, 32), xmm7);
#ifdef _LP64
    __ vmovdqu(Address(rsi, 64), xmm8);
    __ vmovdqu(Address(rsi, 96), xmm15);
#endif

#ifdef _WINDOWS
#ifdef _LP64
    __ vmovdqu(xmm15, Address(rsp, 0));
    __ addptr(rsp, 32);
    __ vmovdqu(xmm8, Address(rsp, 0));
    __ addptr(rsp, 32);
#endif // _LP64
    __ vmovdqu(xmm7, Address(rsp, 0));
    __ addptr(rsp, 32);
#endif // _WINDOWS
    generate_vzeroupper(wrapup);
    VM_Version::clean_cpuFeatures();
    UseAVX = saved_useavx;
    UseSSE = saved_usesse;

    __ bind(wrapup);
    __ popf();
    __ pop(rsi);
    __ pop(rbx);
    __ pop(rbp);
    __ ret(0);

#   undef __

    return start;
  };
  void generate_vzeroupper(Label& L_wrapup) {
#   define __ _masm->
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid0_offset())));
    __ cmpl(Address(rsi, 4), 0x756e6547);  // 'uneG'
    __ jcc(Assembler::notEqual, L_wrapup);
    __ movl(rcx, 0x0FFF0FF0);
    __ lea(rsi, Address(rbp, in_bytes(VM_Version::std_cpuid1_offset())));
    __ andl(rcx, Address(rsi, 0));
    __ cmpl(rcx, 0x00050670);              // If it is Xeon Phi 3200/5200/7200
    __ jcc(Assembler::equal, L_wrapup);
    __ cmpl(rcx, 0x00080650);              // If it is Future Xeon Phi
    __ jcc(Assembler::equal, L_wrapup);
    // vzeroupper() will use a pre-computed instruction sequence that we
    // can't compute until after we've determined CPU capabilities. Use
    // uncached variant here directly to be able to bootstrap correctly
    __ vzeroupper_uncached();
#   undef __
  }
  address generate_detect_virt() {
    StubCodeMark mark(this, "VM_Version", "detect_virt_stub");
#   define __ _masm->

    address start = __ pc();

    // Evacuate callee-saved registers
    __ push(rbp);
    __ push(rbx);
    __ push(rsi); // for Windows

#ifdef _LP64
    __ mov(rax, c_rarg0); // CPUID leaf
    __ mov(rsi, c_rarg1); // register array address (eax, ebx, ecx, edx)
#else
    __ movptr(rax, Address(rsp, 16)); // CPUID leaf
    __ movptr(rsi, Address(rsp, 20)); // register array address
#endif

    __ cpuid();

    // Store result to register array
    __ movl(Address(rsi,  0), rax);
    __ movl(Address(rsi,  4), rbx);
    __ movl(Address(rsi,  8), rcx);
    __ movl(Address(rsi, 12), rdx);

    // Epilogue
    __ pop(rsi);
    __ pop(rbx);
    __ pop(rbp);
    __ ret(0);

#   undef __

    return start;
  };
};

uint64_t VM_Version::CPUFeatures_parse(uint64_t &glibc_features) {
  glibc_features = _glibc_features;
#ifndef LINUX
  ignore_glibc_not_using = true;
  return _features;
#endif
  if (CPUFeatures == NULL || strcmp(CPUFeatures, "native") == 0) {
    return _features;
  }
  if (strcmp(CPUFeatures, "ignore") == 0) {
    ignore_glibc_not_using = true;
    return _features;
  }
  glibc_features = 0;
  if (strcmp(CPUFeatures, "generic") == 0) {
    // 32-bit x86 cannot rely on anything.
    return 0
#ifdef AMD64
      // The following options are all in /proc/cpuinfo of one of the first 64-bit CPUs - Atom D2700 (and Opteron 1352): https://superuser.com/q/1572306/1015048
      | CPU_SSE // enabled in 'gcc -Q --help=target', used by OpenJDK
      | CPU_SSE2 // enabled in 'gcc -Q --help=target', required by OpenJDK
      | CPU_FXSR // enabled in 'gcc -Q --help=target', not used by OpenJDK
      | CPU_MMX // enabled in 'gcc -Q --help=target', used only by 32-bit x86 OpenJDK
      | CPU_TSC // not used by gcc, used by OpenJDK
      | CPU_CX8 // gcc detects it to set cpu "pentium" (=32-bit only), used by OpenJDK
      | CPU_CMOV // gcc detects it to set cpu "pentiumpro" (=32-bit only), used by OpenJDK
      | CPU_FLUSH // ="clflush" in cpuinfo, not used by gcc, required by OpenJDK
      // GLIBC_MOVBE is disabled in 'gcc -Q --help=target' and some CPUs do not support it: https://stackoverflow.com/a/5246553/2995591
      // GLIBC_LAHFSAHF is disabled in 'gcc -Q --help=target' and "Early Intel Pentium 4 CPUs with Intel 64 support ... lacked the LAHF and SAHF instructions"
#endif
    ;
  }
  char *endptr;
  errno = 0;
  uint64_t retval;
  unsigned long long ull = strtoull(CPUFeatures, &endptr, 0);
  retval = ull;
  if (!errno && *endptr == ',' && retval == ull) {
    ull = strtoull(endptr + 1, &endptr, 0);
    glibc_features = ull;
    if (!errno && !*endptr && glibc_features == ull) {
      return retval;
    }
  }
  vm_exit_during_initialization(err_msg("VM option 'CPUFeatures=%s' must be of the form: 0xnum,0xnum", CPUFeatures));
  return -1;
}

#if INCLUDE_LD_SO_LIST_DIAGNOSTICS

static int ld_so_name_iterate_phdr(struct dl_phdr_info *info, size_t size, void *data_voidp) {
  const char **retval_return = (const char **)data_voidp;
  assert(size >= offsetof(struct dl_phdr_info, dlpi_adds), "missing PHDRs for the java executable");
  if (strcmp(info->dlpi_name, "") != 0)
    vm_exit_during_initialization(err_msg("Unexpected name of first dl_phdr_info: %s", info->dlpi_name));
  for (size_t phdr_ix = 0; phdr_ix < info->dlpi_phnum; ++phdr_ix) {
    const Elf64_Phdr *phdr = info->dlpi_phdr + phdr_ix;
    if (phdr->p_type == PT_INTERP) {
      *retval_return = (const char *)(phdr->p_vaddr + info->dlpi_addr);
      return 42;
    }
  }
  vm_exit_during_initialization("PT_INTERP not found for the java executable");
  return -1;
}

static const char *ld_so_name() {
  const char *retval;
  int err = dl_iterate_phdr(ld_so_name_iterate_phdr, &retval);
  assert(err == 42, "internal error 42");
  return retval;
}

#define ARG1 "--list-diagnostics"

static FILE *popen_r(const char *arg0, pid_t *pid_return) {
  union {
    int fds[2];
    struct {
      int readfd, writefd;
    };
  } fds;
  if (pipe(fds.fds))
    vm_exit_during_initialization(err_msg("Error creating pipe: %m"));
  pid_t child = fork();
  switch (child) {
    case -1:
      vm_exit_during_initialization(err_msg("Error fork-ing: %m"));
    case 0:
      if (close(fds.readfd))
        vm_exit_during_initialization(err_msgt("Error closing read pipe in child: %m"));
      }
      if (dup2(fds.writefd, STDOUT_FILENO) != STDOUT_FILENO)
        vm_exit_during_initialization(err_msg("Error closing preparing write pipe in child: %m"));
      if (close(fds.writefd))
        vm_exit_during_initialization(err_msg("Error closing write pipe in child: %m"));
      execl(arg0, arg0, ARG1, NULL);
      // FIXME: Double vm_exit*()?
      vm_exit_during_initialization(err_msg("Error exec-ing %s " ARG1 ": %m", arg0));
  }
  if (close(fds.writefd))
    vm_exit_during_initialization(err_msg("Error closing write pipe in parent: %m"));
  FILE *f = fdopen(fds.readfd, "r");
  if (f == NULL)
    vm_exit_during_initialization(err_msg("Error converting pipe fd to FILE * in parent for %s " ARG1 ": %m", arg0));
  *pid_return = child;
  return f;
}

static void pclose_r(const char *arg0, FILE *f, pid_t pid) {
  if (fclose(f))
    vm_exit_during_initialization(err_msg("Error closing fdopen-ed %s " ARG1 ": %m", arg0));
  int wstatus;
  pid_t waiterr = waitpid(pid, &wstatus, 0);
  if (waiterr != pid)
    vm_exit_during_initialization(err_msg("Error waiting on %s " ARG1 ": %m", arg0));
  if (!WIFEXITED(wstatus))
    vm_exit_during_initialization(err_msg("Child command %s " ARG1 " did not properly exit (WIFEXITED): wstatus = %d", arg0, wstatus));
  if (WEXITSTATUS(wstatus) != 0)
    vm_exit_during_initialization(err_msg("Child command %s " ARG1 " did exit with an error: exit code = %d", arg0, WEXITSTATUS(wstatus)));
}

#endif // !INCLUDE_LD_SO_LIST_DIAGNOSTICS

bool VM_Version::ignore_glibc_not_using = false;
#ifdef LINUX
const char VM_Version::glibc_prefix[] = ":glibc.cpu.hwcaps=";
const size_t VM_Version::glibc_prefix_len = strlen(glibc_prefix);

bool VM_Version::glibc_env_set(char *disable_str) {
#define TUNABLES_NAME "GLIBC_TUNABLES"
  char *env_val = disable_str;
  const char *env = getenv(TUNABLES_NAME);
  if (env && strcmp(env, env_val) == 0) {
#if !INCLUDE_CPU_FEATURE_ACTIVE && !INCLUDE_LD_SO_LIST_DIAGNOSTICS
    if (ShowCPUFeatures) {
      tty->print_cr("Environment variable already set, both glibc CPU_FEATURE_ACTIVE and ld.so --list-diagnostics are unavailable - re-exec suppressed: " TUNABLES_NAME "=%s", env);
    }
    return true;
#endif
  }
  char env_buf[strlen(disable_str) + (!env ? 0 : strlen(env) + 100)];
  if (env) {
    if (ShowCPUFeatures) {
      tty->print_cr("Original environment variable: " TUNABLES_NAME "=%s", env);
    }
    const char *hwcaps = strstr(env, glibc_prefix + 1 /* skip ':' */);
    if (!hwcaps) {
      strcpy(env_buf, env);
      strcat(env_buf, disable_str);
    } else {
      const char *colon = strchr(hwcaps, ':');
      if (!colon) {
        strcpy(env_buf, env);
        strcat(env_buf, disable_str + glibc_prefix_len);
      } else {
        int err = jio_snprintf(env_buf, sizeof(env_buf), "%.*s%s%s", (int)(colon - env), env, disable_str + glibc_prefix_len, colon);
        assert(err >= 0 && (unsigned)err < sizeof(env_buf), "internal error: " TUNABLES_NAME " buffer overflow");
      }
    }
    env_val = env_buf;
  }
  if (ShowCPUFeatures) {
    tty->print_cr("Re-exec of java with new environment variable: " TUNABLES_NAME "=%s", env_val);
  }
  int err = setenv(TUNABLES_NAME, env_val, 1);
  if (err)
    vm_exit_during_initialization(err_msg("setenv " TUNABLES_NAME " error: %m"));

#define REEXEC_NAME "HOTSPOT_GLIBC_TUNABLES_REEXEC"
  if (getenv(REEXEC_NAME))
    vm_exit_during_initialization(err_msg("internal error: " TUNABLES_NAME "=%s failed and " REEXEC_NAME " is set", disable_str));
  if (setenv(REEXEC_NAME, "1", 1))
    vm_exit_during_initialization(err_msg("setenv " REEXEC_NAME " error: %m"));
#undef REEXEC_NAME
#undef TUNABLES_NAME
  return false;
}

void VM_Version::glibc_reexec() {
  char *buf = NULL;
  size_t buf_allocated = 0;
  size_t buf_used = 0;
#define CMDLINE "/proc/self/cmdline"
  int fd = open(CMDLINE, O_RDONLY);
  if (fd == -1)
    vm_exit_during_initialization(err_msg("Cannot open " CMDLINE ": %m"));
  ssize_t got;
  do {
    if (buf_used == buf_allocated) {
      buf_allocated = MAX2(size_t(4096), 2 * buf_allocated);
      buf = (char *)os::realloc(buf, buf_allocated, mtOther);
      if (buf == NULL)
        vm_exit_during_initialization(err_msg(CMDLINE " reading failed allocating %zu bytes", buf_allocated));
    }
    got = read(fd, buf + buf_used, buf_allocated - buf_used);
    if (got == -1)
      vm_exit_during_initialization(err_msg("Cannot read " CMDLINE ": %m"));
    buf_used += got;
  } while (got);
  if (close(fd))
    vm_exit_during_initialization(err_msg("Cannot close " CMDLINE ": %m"));
  char **argv = NULL;
  size_t argv_allocated = 0;
  size_t argv_used = 0;
  char *s = buf;
  while (s <= buf + buf_used) {
    if (argv_used == argv_allocated) {
      argv_allocated = MAX2(size_t(256), 2 * argv_allocated);
      argv = (char **)os::realloc(argv, argv_allocated * sizeof(*argv), mtOther);
      if (argv == NULL)
        vm_exit_during_initialization(err_msg(CMDLINE " reading failed allocating %zu pointers", argv_allocated));
    }
    if (s == buf + buf_used) {
      break;
    }
    argv[argv_used++] = s;
    s += strnlen(s, buf + buf_used - s);
    if (s == buf + buf_used)
      vm_exit_during_initialization("Missing end of string zero while parsing " CMDLINE);
    ++s;
  }
  argv[argv_used] = NULL;
#undef CMDLINE

#define EXEC "/proc/self/exe"
  execv(EXEC, argv);
  vm_exit_during_initialization(err_msg("Cannot re-execute " EXEC ": %m"));
#undef EXEC
}

void VM_Version::glibc_not_using(uint64_t excessive_CPU, uint64_t excessive_GLIBC) {
#ifndef ASSERT
  if (!excessive_CPU && !excessive_GLIBC)
    return;
#endif
#if INCLUDE_LD_SO_LIST_DIAGNOSTICS
  // sysdeps/x86/include/cpu-features.h CPUID_INDEX_14_ECX_0 == 8
  const int CPUID_INDEX_CEIL = 8;
  // /usr/include/bits/platform/x86.h
  enum
  {
    CPUID_INDEX_1 = 0,
    CPUID_INDEX_7,
    CPUID_INDEX_80000001,
    CPUID_INDEX_D_ECX_1,
    CPUID_INDEX_80000007,
    CPUID_INDEX_80000008,
    CPUID_INDEX_7_ECX_1,
    CPUID_INDEX_19,
    CPUID_INDEX_14_ECX_0
  };
  const int index_max = CPUID_INDEX_CEIL + 1;
  enum { eax = 0, ebx, ecx, edx, reg_max };
  unsigned active[index_max][reg_max] = { 0 };
  const char *arg0 = ld_so_name();
  pid_t f_child;
  FILE *f = popen_r(arg0, &f_child);
  if (!f)
    vm_exit_during_initialization(err_msg("Cannot popen %s " ARG1 ": %m", arg0));
  for (;;) {
    char line[LINE_MAX];
    char *s = fgets(line, sizeof(line), f);
    if (!s)
      break;
    s = line;
    // x86.cpu_features.features[0x0].active[0x2]=0x7ed83203
    const char prefix[] = "x86.cpu_features.features[";
    if (strncmp(s, prefix, sizeof(prefix) - 1) != 0)
      continue;
    s += sizeof(prefix) - 1;
    unsigned long index = strtoul(s, &s, 0);
    if (index >= index_max)
      continue;
    const char mid[] = "].active[";
    if (strncmp(s, mid, sizeof(mid) - 1) != 0)
      continue;
    s += sizeof(mid) - 1;
    unsigned long reg = strtoul(s, &s, 0);
    if (reg >= reg_max)
      continue;
    if (s[0] != ']' || s[1] != '=')
      continue;
    s += 2;
    unsigned long val = strtoul(s, &s, 0);
    if (val > UINT_MAX)
      continue;
    if (s[0] != '\n' || s[1] != 0)
      continue;
    active[index][reg] = val;
  }
  if (ferror(f))
    vm_exit_during_initialization(err_msg("Error reading popen-ed %s " ARG1 ": %m", arg0));
  if (!feof(f))
    vm_exit_during_initialization(err_msg("EOF not reached on popen-ed %s " ARG1, arg0));
  pclose_r(arg0, f, f_child);
#undef ARG1
#endif // INCLUDE_LD_SO_LIST_DIAGNOSTICS

  // glibc: sysdeps/x86/get-isa-level.h:
  // glibc: if (CPU_FEATURE_USABLE_P (cpu_features, CMOV)
  // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, CX8)
  // glibc:     && CPU_FEATURE_CPU_P (cpu_features, FPU)
  // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, FXSR)
  // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, MMX)
  // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSE)
  // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSE2))
  // glibc:     isa_level = GNU_PROPERTY_X86_ISA_1_BASELINE;
  if ((_features & CPU_CMOV) &&
      (_features & CPU_CX8) &&
      // FPU is always present on i686+: (_features & CPU_FPU) &&
      (_features & CPU_SSE2))
    // These cannot be disabled by GLIBC_TUNABLES.
    if (excessive_CPU & (CPU_FXSR | CPU_MMX | CPU_SSE)) {
      assert(!(excessive_CPU & CPU_SSE2), "CPU_SSE2 in both _features and excessive_CPU cannot happen");
      // FIXME: The choice should be based on glibc impact, not the feature age.
      // CX8 is i586+, CMOV is i686+ 1995+, SSE2 is 2000+
      excessive_CPU |= CPU_SSE2;
    }
    if ((_features & CPU_FXSR) &&
        (_features & CPU_MMX) &&
        (_features & CPU_SSE)) {
      // glibc: if (CPU_FEATURE_USABLE_P (cpu_features, CMPXCHG16B)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, LAHF64_SAHF64)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, POPCNT)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSE3)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSSE3)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSE4_1)
      // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, SSE4_2))
      // glibc:     isa_level |= GNU_PROPERTY_X86_ISA_1_V2;
      if ((_features & CPU_POPCNT) &&
          (_features & CPU_SSSE3) &&
          (_features & CPU_SSE4_1) &&
          (_features & CPU_SSE4_2)) {
        if ((excessive_CPU & CPU_SSE3) ||
            (excessive_GLIBC & (GLIBC_CMPXCHG16 | GLIBC_LAHFSAHF))) {
          assert(!(excessive_CPU & CPU_SSE4_2), "CPU_SSE4_2 in both _features and excessive_CPU cannot happen");
          // POPCNT is 2007+, SSSE3 is 2006+, SSE4_1 is 2007+, SSE4_2 is 2008+.
          excessive_CPU |= CPU_SSE4_2;
        }
        if ((_features & CPU_SSE3) &&
            (_glibc_features & GLIBC_CMPXCHG16) &&
            (_glibc_features & GLIBC_LAHFSAHF)) {
          // glibc: if (CPU_FEATURE_USABLE_P (cpu_features, AVX)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, AVX2)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, BMI1)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, BMI2)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, F16C)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, FMA)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, LZCNT)
          // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, MOVBE))
          // glibc:     isa_level |= GNU_PROPERTY_X86_ISA_1_V3;
          if ((_features & CPU_AVX) &&
              (_features & CPU_AVX2) &&
              (_features & CPU_BMI1) &&
              (_features & CPU_BMI2) &&
              (_features & CPU_FMA) &&
              (_features & CPU_LZCNT) &&
              (_glibc_features & GLIBC_MOVBE)) {
          if (excessive_GLIBC & GLIBC_F16C) {
            assert(!(excessive_GLIBC & GLIBC_MOVBE), "GLIBC_MOVBE in both _glibc_features and excessive_GLIBC cannot happen");
            // FMA is 2012+, AVX2+BMI1+BMI2+LZCNT are 2013+, MOVBE is 2015+
            excessive_GLIBC |= GLIBC_MOVBE;
          }
          if (_glibc_features & GLIBC_F16C) {
            // glibc: if (CPU_FEATURE_USABLE_P (cpu_features, AVX512F)
            // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, AVX512BW)
            // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, AVX512CD)
            // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, AVX512DQ)
            // glibc:     && CPU_FEATURE_USABLE_P (cpu_features, AVX512VL))
            // glibc:   isa_level |= GNU_PROPERTY_X86_ISA_1_V4;
            // All these flags are supported by GLIBC_DISABLE below.
          }
        }
      }
    }
  }
  uint64_t disable_CPU   = 0;
  uint64_t disable_GLIBC = 0;
#define PASTE_TOKENS3(x, y, z) PASTE_TOKENS(x, PASTE_TOKENS(y, z))
#ifdef ASSERT
  uint64_t excessive_handled_CPU   = 0;
  uint64_t excessive_handled_GLIBC = 0;
  uint64_t disable_handled_CPU   = 0;
  uint64_t disable_handled_GLIBC = 0;
#endif
#define EXCESSIVE_HANDLED(kind, hotspot) do {                                                                                         \
    assert(!(PASTE_TOKENS(excessive_handled_, kind) & PASTE_TOKENS3(kind, _, hotspot)), "already used " STR(kind) "_" STR(hotspot) ); \
    DEBUG_ONLY(PASTE_TOKENS(excessive_handled_, kind) |= PASTE_TOKENS3(kind, _, hotspot));                                            \
  } while (0)
#if INCLUDE_CPU_FEATURE_ACTIVE
# define FEATURE_ACTIVE(glibc, hotspot_field, hotspot_union, glibc_index, glibc_reg) CPU_FEATURE_ACTIVE(glibc)
#elif INCLUDE_LD_SO_LIST_DIAGNOSTICS
# define FEATURE_ACTIVE(glibc, hotspot_field, hotspot_union, glibc_index, glibc_reg) ({ \
    hotspot_union u;                                                                    \
    u.value = active[glibc_index][glibc_reg];                                           \
    u.bits.hotspot_field != 0;                                                          \
  })
#else
# define FEATURE_ACTIVE(glibc, hotspot_field, hotspot_union, glibc_index, glibc_reg) true
#endif
#define EXCESSIVE6(kind, hotspotglibc, hotspot_field, hotspot_union, glibc_index, glibc_reg) do {                                                                      \
    EXCESSIVE_HANDLED(kind, hotspotglibc);                                                                                                                             \
    if (PASTE_TOKENS(excessive_, kind) & PASTE_TOKENS3(kind, _, hotspotglibc) && FEATURE_ACTIVE(hotspotglibc, hotspot_field, hotspot_union, glibc_index, glibc_reg)) { \
      PASTE_TOKENS(disable_, kind) |= PASTE_TOKENS3(kind, _, hotspotglibc);                                                                                            \
    }                                                                                                                                                                  \
  } while (0)
#define EXCESSIVE(kind, hotspotglibc, hotspot_union, def...) EXCESSIVE6(kind, hotspotglibc, hotspot_field, def)
#define DEF_ExtCpuid1Ecx ExtCpuid1Ecx, CPUID_INDEX_80000001, ecx
#define DEF_SefCpuid7Ebx SefCpuid7Ebx, CPUID_INDEX_7       , ebx
#define DEF_SefCpuid7Ecx SefCpuid7Ecx, CPUID_INDEX_7       , ecx
#define DEF_SefCpuid7Edx SefCpuid7Edx, CPUID_INDEX_7       , edx
#define DEF_StdCpuid1Ecx StdCpuid1Ecx, CPUID_INDEX_1       , ecx
#define DEF_StdCpuid1Edx StdCpuid1Edx, CPUID_INDEX_1       , edx
  EXCESSIVE(CPU  , AVX     , avx     , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , CX8     , cmpxchg8, DEF_StdCpuid1Edx);
  EXCESSIVE(CPU  , FMA     , fma     , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , RTM     , rtm     , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX2    , avx2    , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , BMI1    , bmi1    , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , BMI2    , bmi2    , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , CMOV    , cmov    , DEF_StdCpuid1Edx);
  EXCESSIVE(CPU  , ERMS    , erms    , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , SSE2    , sse2    , DEF_StdCpuid1Edx);
  EXCESSIVE(CPU  , LZCNT   , fma4    , DEF_ExtCpuid1Ecx);
  EXCESSIVE(CPU  , SSSE3   , ssse3   , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , POPCNT  , popcnt  , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , SSE4_1  , sse4_1  , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , SSE4_2  , sse4_2  , DEF_StdCpuid1Ecx);
  EXCESSIVE(CPU  , AVX512F , avx512f , DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512CD, avx512cd, DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512BW, avx512bw, DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512DQ, avx512dq, DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512ER, avx512er, DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512PF, avx512pf, DEF_SefCpuid7Ebx);
  EXCESSIVE(CPU  , AVX512VL, avx512vl, DEF_SefCpuid7Ebx);
  EXCESSIVE(GLIBC, IBT     , ibt     , DEF_SefCpuid7Edx);
  EXCESSIVE(GLIBC, FMA4    , fma4    , DEF_ExtCpuid1Ecx);
  EXCESSIVE(GLIBC, MOVBE   , movbe   , DEF_StdCpuid1Ecx);
  EXCESSIVE(GLIBC, SHSTK   , shstk   , DEF_SefCpuid7Ecx);
  EXCESSIVE(GLIBC, XSAVE   , xsave   , DEF_StdCpuid1Ecx);
  EXCESSIVE(GLIBC, OSXSAVE , osxsave , DEF_StdCpuid1Ecx);
  EXCESSIVE(GLIBC, HTT     , ht      , DEF_StdCpuid1Edx);
#undef EXCESSIVE
#undef EXCESSIVE5

  char disable_str[64 * (10 + 3) + 1];
  strcpy(disable_str, glibc_prefix);
  char *disable_end = disable_str + glibc_prefix_len;
#define GLIBC_DISABLE(kind, hotspot_glibc) do {                                                                                                 \
    assert(!(PASTE_TOKENS(disable_handled_, kind) & PASTE_TOKENS3(kind, _, hotspot_glibc)), "already used " STR(kind) "_" STR(hotspot_glibc) ); \
    DEBUG_ONLY(PASTE_TOKENS(disable_handled_, kind) |= PASTE_TOKENS3(kind, _, hotspot_glibc));                                                  \
    if (PASTE_TOKENS(disable_, kind) & PASTE_TOKENS3(kind, _, hotspot_glibc)) {                                                                 \
      const char str[] = ",-" STR(hotspot_glibc);                                                                                               \
      size_t remains = disable_str + sizeof(disable_str) - disable_end;                                                                         \
      strncpy(disable_end, str, remains);                                                                                                       \
      size_t len = strnlen(disable_end, remains);                                                                                               \
      remains -= len;                                                                                                                           \
      assert(remains > 0, "internal error: disable_str overflow");                                                                              \
      disable_end += len;                                                                                                                       \
    }                                                                                                                                           \
  } while (0);
  GLIBC_DISABLE(CPU  , AVX)
  GLIBC_DISABLE(CPU  , CX8)
  GLIBC_DISABLE(CPU  , FMA)
  GLIBC_DISABLE(CPU  , RTM)
  GLIBC_DISABLE(CPU  , AVX2)
  GLIBC_DISABLE(CPU  , BMI1)
  GLIBC_DISABLE(CPU  , BMI2)
  GLIBC_DISABLE(CPU  , CMOV)
  GLIBC_DISABLE(CPU  , ERMS)
  GLIBC_DISABLE(CPU  , SSE2)
  GLIBC_DISABLE(CPU  , LZCNT)
  GLIBC_DISABLE(CPU  , SSSE3)
  GLIBC_DISABLE(CPU  , POPCNT)
  GLIBC_DISABLE(CPU  , SSE4_1)
  GLIBC_DISABLE(CPU  , SSE4_2)
  GLIBC_DISABLE(CPU  , AVX512F)
  GLIBC_DISABLE(CPU  , AVX512CD)
  GLIBC_DISABLE(CPU  , AVX512BW)
  GLIBC_DISABLE(CPU  , AVX512DQ)
  GLIBC_DISABLE(CPU  , AVX512ER)
  GLIBC_DISABLE(CPU  , AVX512PF)
  GLIBC_DISABLE(CPU  , AVX512VL)
  GLIBC_DISABLE(GLIBC, IBT)
  GLIBC_DISABLE(GLIBC, FMA4)
  GLIBC_DISABLE(GLIBC, MOVBE)
  GLIBC_DISABLE(GLIBC, SHSTK)
  GLIBC_DISABLE(GLIBC, XSAVE)
  GLIBC_DISABLE(GLIBC, OSXSAVE)
  GLIBC_DISABLE(GLIBC, HTT)
#undef GLIBC_DISABLE
#undef GLIBC_DISABLE2
  *disable_end = 0;

#ifdef ASSERT
#define CHECK_KIND(kind) do {                                                                                                            \
    if (PASTE_TOKENS(disable_handled_, kind) != PASTE_TOKENS(excessive_handled_, kind))                                                  \
      vm_exit_during_initialization(err_msg("internal error: Unsupported disabling of " STR(kind) "_* 0x%" PRIx64 " != used 0x%" PRIx64, \
                                            PASTE_TOKENS(disable_handled_, kind), PASTE_TOKENS(excessive_handled_, kind)));              \
  } while (0)
  CHECK_KIND(CPU  );
  CHECK_KIND(GLIBC);
#undef CHECK_KIND

  // These cannot be disabled by GLIBC_TUNABLES interface.
#define GLIBC_UNSUPPORTED(kind, hotspot) EXCESSIVE_HANDLED(kind, hotspot)
  GLIBC_UNSUPPORTED(CPU  , 3DNOW_PREFETCH   );
  GLIBC_UNSUPPORTED(CPU  , SSE4A            );
  GLIBC_UNSUPPORTED(CPU  , TSC              );
  GLIBC_UNSUPPORTED(CPU  , TSCINV_BIT       );
  GLIBC_UNSUPPORTED(CPU  , TSCINV           );
  GLIBC_UNSUPPORTED(CPU  , AES              );
  GLIBC_UNSUPPORTED(CPU  , CLMUL            );
  GLIBC_UNSUPPORTED(CPU  , ADX              );
  GLIBC_UNSUPPORTED(CPU  , SHA              );
  GLIBC_UNSUPPORTED(CPU  , VZEROUPPER       );
  GLIBC_UNSUPPORTED(CPU  , AVX512_VPOPCNTDQ );
  GLIBC_UNSUPPORTED(CPU  , AVX512_VPCLMULQDQ);
  GLIBC_UNSUPPORTED(CPU  , AVX512_VAES      );
  GLIBC_UNSUPPORTED(CPU  , AVX512_VNNI      );
  GLIBC_UNSUPPORTED(CPU  , FLUSH            );
  GLIBC_UNSUPPORTED(CPU  , FLUSHOPT         );
  GLIBC_UNSUPPORTED(CPU  , CLWB             );
  GLIBC_UNSUPPORTED(CPU  , AVX512_VBMI2     );
  GLIBC_UNSUPPORTED(CPU  , AVX512_VBMI      );
  GLIBC_UNSUPPORTED(CPU  , HV               );
  GLIBC_UNSUPPORTED(CPU  , SSE3             );
  // These are handled as an exception above.
  GLIBC_UNSUPPORTED(CPU  , FXSR             );
  GLIBC_UNSUPPORTED(CPU  , MMX              );
  GLIBC_UNSUPPORTED(CPU  , SSE              );
  GLIBC_UNSUPPORTED(CPU  , HT               );
  GLIBC_UNSUPPORTED(GLIBC, CMPXCHG16        );
  GLIBC_UNSUPPORTED(GLIBC, LAHFSAHF         );
  GLIBC_UNSUPPORTED(GLIBC, F16C             );
#undef GLIBC_UNSUPPORTED
#define CHECK_KIND(kind) do {                                                                                                                 \
    if (PASTE_TOKENS(excessive_handled_, kind) != PASTE_TOKENS(MAX_, kind) - 1)                                                               \
      vm_exit_during_initialization(err_msg("internal error: Unsupported disabling of some " STR(kind) "_* 0x%" PRIx64 " != full 0x%" PRIx64, \
                                            PASTE_TOKENS(excessive_handled_, kind), PASTE_TOKENS(MAX_, kind) - 1));                           \
  } while (0)
  CHECK_KIND(CPU  );
  CHECK_KIND(GLIBC);
#undef CHECK_KIND
#endif // ASSERT

  if (disable_end == disable_str + glibc_prefix_len)
    return;
  if (glibc_env_set(disable_str))
    return;
  glibc_reexec();
}
#endif //LINUX

void VM_Version::nonlibc_tty_print_uint64(uint64_t num) {
  static const char prefix[] = "0x";
  tty->write(prefix, sizeof(prefix) - 1);
  bool first = true;
  for (int pos = 64 - 4; pos >= 0; pos -= 4) {
    unsigned nibble = (num >> pos) & 0xf;
    if (first && nibble == 0 && pos)
      continue;
    first = false;
    char c = nibble >= 0xa ? 'a' + nibble - 0xa : '0' + nibble;
    tty->write(&c, sizeof(c));
  }
}

void VM_Version::nonlibc_tty_print_uint64_comma_uint64(uint64_t num1, uint64_t num2) {
  nonlibc_tty_print_uint64(num1);
  static const char comma = ',';
  tty->print_raw(&comma, sizeof(comma));
  nonlibc_tty_print_uint64(num2);
}

void VM_Version::print_using_features_cr() {
  if (ignore_glibc_not_using) {
    tty->print_cr("CPU features are being kept intact as requested by -XX:CPUFeatures=ignore");
  } else {
    tty->print_cr("CPU features being used are: -XX:CPUFeatures=0x" UINT64_FORMAT_X ",0x" UINT64_FORMAT_X, _features, _glibc_features);
  }
}

void VM_Version::get_processor_features_hardware() {
  _cpu = 4; // 486 by default
  _model = 0;
  _stepping = 0;
  _features = 0;
  _logical_processors_per_package = 1;
  // i486 internal cache is both I&D and has a 16-byte line size
  _L1_data_cache_line_size = 16;

  // Get raw processor info

  get_cpu_info_stub(&_cpuid_info);

  assert_is_initialized();
  _cpu = extended_cpu_family();
  _model = extended_cpu_model();
  _stepping = cpu_stepping();

  if (cpu_family() > 4) { // it supports CPUID
    _features = feature_flags();
    LINUX_ONLY(_glibc_features = glibc_flags();)
    // Logical processors are only available on P4s and above,
    // and only if hyperthreading is available.
    _logical_processors_per_package = logical_processor_count();
    _L1_data_cache_line_size = L1_line_size();
  }

  _supports_cx8 = supports_cmpxchg8();
  // xchg and xadd instructions
  _supports_atomic_getset4 = true;
  _supports_atomic_getadd4 = true;
  LP64_ONLY(_supports_atomic_getset8 = true);
  LP64_ONLY(_supports_atomic_getadd8 = true);

  if (ShowCPUFeatures) {
    static const char prefix[] = "This machine's CPU features are: -XX:CPUFeatures=";
    tty->print_raw(prefix, sizeof(prefix) - 1);
    nonlibc_tty_print_uint64_comma_uint64(_features, _glibc_features);
    tty->cr();
  }
}

void VM_Version::get_processor_features_hotspot() {
#ifdef _LP64
  // OS should support SSE for x64 and hardware should support at least SSE2.
  if (!VM_Version::supports_sse2()) {
    if (!FLAG_IS_DEFAULT(CPUFeatures))
      vm_exit_during_initialization(err_msg("-XX:CPUFeatures option requires SSE2 flag to be set: 0x%" PRIx64 ",0x%" PRIx64, CPU_SSE2, (uint64_t)0));
    vm_exit_during_initialization("Unknown x64 processor: SSE2 not supported");
  }
  // in 64 bit the use of SSE2 is the minimum
  if (UseSSE < 2) UseSSE = 2;
#endif

#ifdef AMD64
  // flush_icache_stub have to be generated first.
  // That is why Icache line size is hard coded in ICache class,
  // see icache_x86.hpp. It is also the reason why we can't use
  // clflush instruction in 32-bit VM since it could be running
  // on CPU which does not support it.
  //
  // The only thing we can do is to verify that flushed
  // ICache::line_size has correct value.
  guarantee(_cpuid_info.std_cpuid1_edx.bits.clflush != 0, "clflush is not supported");
  // clflush_size is size in quadwords (8 bytes).
  guarantee(_cpuid_info.std_cpuid1_ebx.bits.clflush_size == 8, "such clflush size is not supported");
#endif

#ifdef _LP64
  // assigning this field effectively enables Unsafe.writebackMemory()
  // by initing UnsafeConstant.DATA_CACHE_LINE_FLUSH_SIZE to non-zero
  // that is only implemented on x86_64 and only if the OS plays ball
  if (os::supports_map_sync()) {
    // publish data cache line flush size to generic field, otherwise
    // let if default to zero thereby disabling writeback
    _data_cache_line_flush_size = _cpuid_info.std_cpuid1_ebx.bits.clflush_size * 8;
  }
#endif
  // If the OS doesn't support SSE, we can't use this feature even if the HW does
  if (!os::supports_sse())
    _features &= ~(CPU_SSE|CPU_SSE2|CPU_SSE3|CPU_SSSE3|CPU_SSE4A|CPU_SSE4_1|CPU_SSE4_2);

  if (UseSSE < 4) {
    _features &= ~CPU_SSE4_1;
    _features &= ~CPU_SSE4_2;
  }

  if (UseSSE < 3) {
    _features &= ~CPU_SSE3;
    _features &= ~CPU_SSSE3;
    _features &= ~CPU_SSE4A;
  }

  if (UseSSE < 2)
    _features &= ~CPU_SSE2;

  if (UseSSE < 1)
    _features &= ~CPU_SSE;

  //since AVX instructions is slower than SSE in some ZX cpus, force USEAVX=0.
  if (is_zx() && ((cpu_family() == 6) || (cpu_family() == 7))) {
    UseAVX = 0;
  }

  // first try initial setting and detect what we can support
  int use_avx_limit = 0;
  if (UseAVX > 0) {
    if (UseAVX > 2 && supports_evex()) {
      use_avx_limit = 3;
    } else if (UseAVX > 1 && supports_avx2()) {
      use_avx_limit = 2;
    } else if (UseAVX > 0 && supports_avx()) {
      use_avx_limit = 1;
    } else {
      use_avx_limit = 0;
    }
  }
  if (FLAG_IS_DEFAULT(UseAVX)) {
    // Don't use AVX-512 on older Skylakes unless explicitly requested.
    if (use_avx_limit > 2 && is_intel_skylake() && _stepping < 5) {
      FLAG_SET_DEFAULT(UseAVX, 2);
    } else {
      FLAG_SET_DEFAULT(UseAVX, use_avx_limit);
    }
  }
  if (UseAVX > use_avx_limit) {
    warning("UseAVX=%d is not supported on this CPU, setting it to UseAVX=%d", (int) UseAVX, use_avx_limit);
    FLAG_SET_DEFAULT(UseAVX, use_avx_limit);
  } else if (UseAVX < 0) {
    warning("UseAVX=%d is not valid, setting it to UseAVX=0", (int) UseAVX);
    FLAG_SET_DEFAULT(UseAVX, 0);
  }

  if (UseAVX < 3) {
    _features &= ~CPU_AVX512F;
    _features &= ~CPU_AVX512DQ;
    _features &= ~CPU_AVX512CD;
    _features &= ~CPU_AVX512BW;
    _features &= ~CPU_AVX512VL;
    _features &= ~CPU_AVX512_VPOPCNTDQ;
    _features &= ~CPU_AVX512_VPCLMULQDQ;
    _features &= ~CPU_AVX512_VAES;
    _features &= ~CPU_AVX512_VNNI;
    _features &= ~CPU_AVX512_VBMI;
    _features &= ~CPU_AVX512_VBMI2;
  }

  if (UseAVX < 2)
    _features &= ~CPU_AVX2;

  if (UseAVX < 1) {
    _features &= ~CPU_AVX;
    _features &= ~CPU_VZEROUPPER;
  }

  if (logical_processors_per_package() == 1) {
    // HT processor could be installed on a system which doesn't support HT.
    _features &= ~CPU_HT;
  }

  if (is_intel()) { // Intel cpus specific settings
    if (is_knights_family()) {
      _features &= ~CPU_VZEROUPPER;
      _features &= ~CPU_AVX512BW;
      _features &= ~CPU_AVX512VL;
      _features &= ~CPU_AVX512DQ;
      _features &= ~CPU_AVX512_VNNI;
      _features &= ~CPU_AVX512_VAES;
      _features &= ~CPU_AVX512_VPOPCNTDQ;
      _features &= ~CPU_AVX512_VPCLMULQDQ;
      _features &= ~CPU_AVX512_VBMI;
      _features &= ~CPU_AVX512_VBMI2;
      _features &= ~CPU_CLWB;
      _features &= ~CPU_FLUSHOPT;
    }
  }

  if (FLAG_IS_DEFAULT(IntelJccErratumMitigation)) {
    _has_intel_jcc_erratum = compute_has_intel_jcc_erratum();
  } else {
    _has_intel_jcc_erratum = IntelJccErratumMitigation;
  }

  char buf[512];
  int res = jio_snprintf(
              buf, sizeof(buf),
              "(%u cores per cpu, %u threads per core) family %d model %d stepping %d microcode 0x%x",
              cores_per_cpu(), threads_per_core(),
              cpu_family(), _model, _stepping, os::cpu_microcode_revision());
  assert(res > 0, "not enough temporary space allocated");
  insert_features_names(buf + res, sizeof(buf) - res);

  _features_string = os::strdup(buf);

  // UseSSE is set to the smaller of what hardware supports and what
  // the command line requires.  I.e., you cannot set UseSSE to 2 on
  // older Pentiums which do not support it.
  int use_sse_limit = 0;
  if (UseSSE > 0) {
    if (UseSSE > 3 && supports_sse4_1()) {
      use_sse_limit = 4;
    } else if (UseSSE > 2 && supports_sse3()) {
      use_sse_limit = 3;
    } else if (UseSSE > 1 && supports_sse2()) {
      use_sse_limit = 2;
    } else if (UseSSE > 0 && supports_sse()) {
      use_sse_limit = 1;
    } else {
      use_sse_limit = 0;
    }
  }
  if (FLAG_IS_DEFAULT(UseSSE)) {
    FLAG_SET_DEFAULT(UseSSE, use_sse_limit);
  } else if (UseSSE > use_sse_limit) {
    warning("UseSSE=%d is not supported on this CPU, setting it to UseSSE=%d", (int) UseSSE, use_sse_limit);
    FLAG_SET_DEFAULT(UseSSE, use_sse_limit);
  } else if (UseSSE < 0) {
    warning("UseSSE=%d is not valid, setting it to UseSSE=0", (int) UseSSE);
    FLAG_SET_DEFAULT(UseSSE, 0);
  }

  // Use AES instructions if available.
  if (supports_aes()) {
    if (FLAG_IS_DEFAULT(UseAES)) {
      FLAG_SET_DEFAULT(UseAES, true);
    }
    if (!UseAES) {
      if (UseAESIntrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
        warning("AES intrinsics require UseAES flag to be enabled. Intrinsics will be disabled.");
      }
      FLAG_SET_DEFAULT(UseAESIntrinsics, false);
    } else {
      if (UseSSE > 2) {
        if (FLAG_IS_DEFAULT(UseAESIntrinsics)) {
          FLAG_SET_DEFAULT(UseAESIntrinsics, true);
        }
      } else {
        // The AES intrinsic stubs require AES instruction support (of course)
        // but also require sse3 mode or higher for instructions it use.
        if (UseAESIntrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
          warning("X86 AES intrinsics require SSE3 instructions or higher. Intrinsics will be disabled.");
        }
        FLAG_SET_DEFAULT(UseAESIntrinsics, false);
      }

      // --AES-CTR begins--
      if (!UseAESIntrinsics) {
        if (UseAESCTRIntrinsics && !FLAG_IS_DEFAULT(UseAESCTRIntrinsics)) {
          warning("AES-CTR intrinsics require UseAESIntrinsics flag to be enabled. Intrinsics will be disabled.");
          FLAG_SET_DEFAULT(UseAESCTRIntrinsics, false);
        }
      } else {
        if (supports_sse4_1()) {
          if (FLAG_IS_DEFAULT(UseAESCTRIntrinsics)) {
            FLAG_SET_DEFAULT(UseAESCTRIntrinsics, true);
          }
        } else {
           // The AES-CTR intrinsic stubs require AES instruction support (of course)
           // but also require sse4.1 mode or higher for instructions it use.
          if (UseAESCTRIntrinsics && !FLAG_IS_DEFAULT(UseAESCTRIntrinsics)) {
             warning("X86 AES-CTR intrinsics require SSE4.1 instructions or higher. Intrinsics will be disabled.");
           }
           FLAG_SET_DEFAULT(UseAESCTRIntrinsics, false);
        }
      }
      // --AES-CTR ends--
    }
  } else if (UseAES || UseAESIntrinsics || UseAESCTRIntrinsics) {
    if (UseAES && !FLAG_IS_DEFAULT(UseAES)) {
      warning("AES instructions are not available on this CPU");
      FLAG_SET_DEFAULT(UseAES, false);
    }
    if (UseAESIntrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
      warning("AES intrinsics are not available on this CPU");
      FLAG_SET_DEFAULT(UseAESIntrinsics, false);
    }
    if (UseAESCTRIntrinsics && !FLAG_IS_DEFAULT(UseAESCTRIntrinsics)) {
      warning("AES-CTR intrinsics are not available on this CPU");
      FLAG_SET_DEFAULT(UseAESCTRIntrinsics, false);
    }
  }

  // Use CLMUL instructions if available.
  if (supports_clmul()) {
    if (FLAG_IS_DEFAULT(UseCLMUL)) {
      UseCLMUL = true;
    }
  } else if (UseCLMUL) {
    if (!FLAG_IS_DEFAULT(UseCLMUL))
      warning("CLMUL instructions not available on this CPU (AVX may also be required)");
    FLAG_SET_DEFAULT(UseCLMUL, false);
  }

  if (UseCLMUL && (UseSSE > 2)) {
    if (FLAG_IS_DEFAULT(UseCRC32Intrinsics)) {
      UseCRC32Intrinsics = true;
    }
  } else if (UseCRC32Intrinsics) {
    if (!FLAG_IS_DEFAULT(UseCRC32Intrinsics))
      warning("CRC32 Intrinsics requires CLMUL instructions (not available on this CPU)");
    FLAG_SET_DEFAULT(UseCRC32Intrinsics, false);
  }

#ifdef _LP64
  if (supports_avx2()) {
    if (FLAG_IS_DEFAULT(UseAdler32Intrinsics)) {
      UseAdler32Intrinsics = true;
    }
  } else if (UseAdler32Intrinsics) {
    if (!FLAG_IS_DEFAULT(UseAdler32Intrinsics)) {
      warning("Adler32 Intrinsics requires avx2 instructions (not available on this CPU)");
    }
    FLAG_SET_DEFAULT(UseAdler32Intrinsics, false);
  }
#else
  if (UseAdler32Intrinsics) {
    warning("Adler32Intrinsics not available on this CPU.");
    FLAG_SET_DEFAULT(UseAdler32Intrinsics, false);
  }
#endif

  if (supports_sse4_2() && supports_clmul()) {
    if (FLAG_IS_DEFAULT(UseCRC32CIntrinsics)) {
      UseCRC32CIntrinsics = true;
    }
  } else if (UseCRC32CIntrinsics) {
    if (!FLAG_IS_DEFAULT(UseCRC32CIntrinsics)) {
      warning("CRC32C intrinsics are not available on this CPU");
    }
    FLAG_SET_DEFAULT(UseCRC32CIntrinsics, false);
  }

  // GHASH/GCM intrinsics
  if (UseCLMUL && (UseSSE > 2)) {
    if (FLAG_IS_DEFAULT(UseGHASHIntrinsics)) {
      UseGHASHIntrinsics = true;
    }
  } else if (UseGHASHIntrinsics) {
    if (!FLAG_IS_DEFAULT(UseGHASHIntrinsics))
      warning("GHASH intrinsic requires CLMUL and SSE2 instructions on this CPU");
    FLAG_SET_DEFAULT(UseGHASHIntrinsics, false);
  }

  // Base64 Intrinsics (Check the condition for which the intrinsic will be active)
  if ((UseAVX > 2) && supports_avx512vl() && supports_avx512bw()) {
    if (FLAG_IS_DEFAULT(UseBASE64Intrinsics)) {
      UseBASE64Intrinsics = true;
    }
  } else if (UseBASE64Intrinsics) {
     if (!FLAG_IS_DEFAULT(UseBASE64Intrinsics))
      warning("Base64 intrinsic requires EVEX instructions on this CPU");
    FLAG_SET_DEFAULT(UseBASE64Intrinsics, false);
  }

  if (supports_fma() && UseSSE >= 2) { // Check UseSSE since FMA code uses SSE instructions
    if (FLAG_IS_DEFAULT(UseFMA)) {
      UseFMA = true;
    }
  } else if (UseFMA) {
    warning("FMA instructions are not available on this CPU");
    FLAG_SET_DEFAULT(UseFMA, false);
  }

  if (FLAG_IS_DEFAULT(UseMD5Intrinsics)) {
    UseMD5Intrinsics = true;
  }

  if (supports_sha() LP64_ONLY(|| supports_avx2() && supports_bmi2())) {
    if (FLAG_IS_DEFAULT(UseSHA)) {
      UseSHA = true;
    }
  } else if (UseSHA) {
    warning("SHA instructions are not available on this CPU");
    FLAG_SET_DEFAULT(UseSHA, false);
  }

  if (supports_sha() && supports_sse4_1() && UseSHA) {
    if (FLAG_IS_DEFAULT(UseSHA1Intrinsics)) {
      FLAG_SET_DEFAULT(UseSHA1Intrinsics, true);
    }
  } else if (UseSHA1Intrinsics) {
    warning("Intrinsics for SHA-1 crypto hash functions not available on this CPU.");
    FLAG_SET_DEFAULT(UseSHA1Intrinsics, false);
  }

  if (supports_sse4_1() && UseSHA) {
    if (FLAG_IS_DEFAULT(UseSHA256Intrinsics)) {
      FLAG_SET_DEFAULT(UseSHA256Intrinsics, true);
    }
  } else if (UseSHA256Intrinsics) {
    warning("Intrinsics for SHA-224 and SHA-256 crypto hash functions not available on this CPU.");
    FLAG_SET_DEFAULT(UseSHA256Intrinsics, false);
  }

#ifdef _LP64
  // These are only supported on 64-bit
  if (UseSHA && supports_avx2() && supports_bmi2()) {
    if (FLAG_IS_DEFAULT(UseSHA512Intrinsics)) {
      FLAG_SET_DEFAULT(UseSHA512Intrinsics, true);
    }
  } else
#endif
  if (UseSHA512Intrinsics) {
    warning("Intrinsics for SHA-384 and SHA-512 crypto hash functions not available on this CPU.");
    FLAG_SET_DEFAULT(UseSHA512Intrinsics, false);
  }

  if (UseSHA3Intrinsics) {
    warning("Intrinsics for SHA3-224, SHA3-256, SHA3-384 and SHA3-512 crypto hash functions not available on this CPU.");
    FLAG_SET_DEFAULT(UseSHA3Intrinsics, false);
  }

  if (!(UseSHA1Intrinsics || UseSHA256Intrinsics || UseSHA512Intrinsics)) {
    FLAG_SET_DEFAULT(UseSHA, false);
  }

  if (!supports_rtm() && UseRTMLocking) {
    // Can't continue because UseRTMLocking affects UseBiasedLocking flag
    // setting during arguments processing. See use_biased_locking().
    // VM_Version_init() is executed after UseBiasedLocking is used
    // in Thread::allocate().
    vm_exit_during_initialization("RTM instructions are not available on this CPU");
  }

#if INCLUDE_RTM_OPT
  if (UseRTMLocking) {
    if (!CompilerConfig::is_c2_enabled()) {
      // Only C2 does RTM locking optimization.
      // Can't continue because UseRTMLocking affects UseBiasedLocking flag
      // setting during arguments processing. See use_biased_locking().
      vm_exit_during_initialization("RTM locking optimization is not supported in this VM");
    }
    if (is_intel_family_core()) {
      if ((_model == CPU_MODEL_HASWELL_E3) ||
          (_model == CPU_MODEL_HASWELL_E7 && _stepping < 3) ||
          (_model == CPU_MODEL_BROADWELL  && _stepping < 4)) {
        // currently a collision between SKL and HSW_E3
        if (!UnlockExperimentalVMOptions && UseAVX < 3) {
          vm_exit_during_initialization("UseRTMLocking is only available as experimental option on this "
                                        "platform. It must be enabled via -XX:+UnlockExperimentalVMOptions flag.");
        } else {
          warning("UseRTMLocking is only available as experimental option on this platform.");
        }
      }
    }
    if (!FLAG_IS_CMDLINE(UseRTMLocking)) {
      // RTM locking should be used only for applications with
      // high lock contention. For now we do not use it by default.
      vm_exit_during_initialization("UseRTMLocking flag should be only set on command line");
    }
  } else { // !UseRTMLocking
    if (UseRTMForStackLocks) {
      if (!FLAG_IS_DEFAULT(UseRTMForStackLocks)) {
        warning("UseRTMForStackLocks flag should be off when UseRTMLocking flag is off");
      }
      FLAG_SET_DEFAULT(UseRTMForStackLocks, false);
    }
    if (UseRTMDeopt) {
      FLAG_SET_DEFAULT(UseRTMDeopt, false);
    }
    if (PrintPreciseRTMLockingStatistics) {
      FLAG_SET_DEFAULT(PrintPreciseRTMLockingStatistics, false);
    }
  }
#else
  if (UseRTMLocking) {
    // Only C2 does RTM locking optimization.
    // Can't continue because UseRTMLocking affects UseBiasedLocking flag
    // setting during arguments processing. See use_biased_locking().
    vm_exit_during_initialization("RTM locking optimization is not supported in this VM");
  }
#endif

#ifdef COMPILER2
  if (UseFPUForSpilling) {
    if (UseSSE < 2) {
      // Only supported with SSE2+
      FLAG_SET_DEFAULT(UseFPUForSpilling, false);
    }
  }
#endif

#if COMPILER2_OR_JVMCI
  int max_vector_size = 0;
  if (UseSSE < 2) {
    // Vectors (in XMM) are only supported with SSE2+
    // SSE is always 2 on x64.
    max_vector_size = 0;
  } else if (UseAVX == 0 || !os_supports_avx_vectors()) {
    // 16 byte vectors (in XMM) are supported with SSE2+
    max_vector_size = 16;
  } else if (UseAVX == 1 || UseAVX == 2) {
    // 32 bytes vectors (in YMM) are only supported with AVX+
    max_vector_size = 32;
  } else if (UseAVX > 2) {
    // 64 bytes vectors (in ZMM) are only supported with AVX 3
    max_vector_size = 64;
  }

#ifdef _LP64
  int min_vector_size = 4; // We require MaxVectorSize to be at least 4 on 64bit
#else
  int min_vector_size = 0;
#endif

  if (!FLAG_IS_DEFAULT(MaxVectorSize)) {
    if (MaxVectorSize < min_vector_size) {
      warning("MaxVectorSize must be at least %i on this platform", min_vector_size);
      FLAG_SET_DEFAULT(MaxVectorSize, min_vector_size);
    }
    if (MaxVectorSize > max_vector_size) {
      warning("MaxVectorSize must be at most %i on this platform", max_vector_size);
      FLAG_SET_DEFAULT(MaxVectorSize, max_vector_size);
    }
    if (!is_power_of_2(MaxVectorSize)) {
      warning("MaxVectorSize must be a power of 2, setting to default: %i", max_vector_size);
      FLAG_SET_DEFAULT(MaxVectorSize, max_vector_size);
    }
  } else {
    // If default, use highest supported configuration
    FLAG_SET_DEFAULT(MaxVectorSize, max_vector_size);
  }

#if defined(COMPILER2) && defined(ASSERT)
  if (MaxVectorSize > 0) {
    if (supports_avx() && PrintMiscellaneous && Verbose && TraceNewVectors) {
      tty->print_cr("State of YMM registers after signal handle:");
      int nreg = 2 LP64_ONLY(+2);
      const char* ymm_name[4] = {"0", "7", "8", "15"};
      for (int i = 0; i < nreg; i++) {
        tty->print("YMM%s:", ymm_name[i]);
        for (int j = 7; j >=0; j--) {
          tty->print(" %x", _cpuid_info.ymm_save[i*8 + j]);
        }
        tty->cr();
      }
    }
  }
#endif // COMPILER2 && ASSERT

#ifdef _LP64
  if (FLAG_IS_DEFAULT(UseMultiplyToLenIntrinsic)) {
    UseMultiplyToLenIntrinsic = true;
  }
  if (FLAG_IS_DEFAULT(UseSquareToLenIntrinsic)) {
    UseSquareToLenIntrinsic = true;
  }
  if (FLAG_IS_DEFAULT(UseMulAddIntrinsic)) {
    UseMulAddIntrinsic = true;
  }
  if (FLAG_IS_DEFAULT(UseMontgomeryMultiplyIntrinsic)) {
    UseMontgomeryMultiplyIntrinsic = true;
  }
  if (FLAG_IS_DEFAULT(UseMontgomerySquareIntrinsic)) {
    UseMontgomerySquareIntrinsic = true;
  }
#else
  if (UseMultiplyToLenIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseMultiplyToLenIntrinsic)) {
      warning("multiplyToLen intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseMultiplyToLenIntrinsic, false);
  }
  if (UseMontgomeryMultiplyIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseMontgomeryMultiplyIntrinsic)) {
      warning("montgomeryMultiply intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseMontgomeryMultiplyIntrinsic, false);
  }
  if (UseMontgomerySquareIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseMontgomerySquareIntrinsic)) {
      warning("montgomerySquare intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseMontgomerySquareIntrinsic, false);
  }
  if (UseSquareToLenIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseSquareToLenIntrinsic)) {
      warning("squareToLen intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseSquareToLenIntrinsic, false);
  }
  if (UseMulAddIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseMulAddIntrinsic)) {
      warning("mulAdd intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseMulAddIntrinsic, false);
  }
#endif // _LP64
#endif // COMPILER2_OR_JVMCI

  // On new cpus instructions which update whole XMM register should be used
  // to prevent partial register stall due to dependencies on high half.
  //
  // UseXmmLoadAndClearUpper == true  --> movsd(xmm, mem)
  // UseXmmLoadAndClearUpper == false --> movlpd(xmm, mem)
  // UseXmmRegToRegMoveAll == true  --> movaps(xmm, xmm), movapd(xmm, xmm).
  // UseXmmRegToRegMoveAll == false --> movss(xmm, xmm),  movsd(xmm, xmm).


  if (is_zx()) { // ZX cpus specific settings
    if (FLAG_IS_DEFAULT(UseStoreImmI16)) {
      UseStoreImmI16 = false; // don't use it on ZX cpus
    }
    if ((cpu_family() == 6) || (cpu_family() == 7)) {
      if (FLAG_IS_DEFAULT(UseAddressNop)) {
        // Use it on all ZX cpus
        UseAddressNop = true;
      }
    }
    if (FLAG_IS_DEFAULT(UseXmmLoadAndClearUpper)) {
      UseXmmLoadAndClearUpper = true; // use movsd on all ZX cpus
    }
    if (FLAG_IS_DEFAULT(UseXmmRegToRegMoveAll)) {
      if (supports_sse3()) {
        UseXmmRegToRegMoveAll = true; // use movaps, movapd on new ZX cpus
      } else {
        UseXmmRegToRegMoveAll = false;
      }
    }
    if (((cpu_family() == 6) || (cpu_family() == 7)) && supports_sse3()) { // new ZX cpus
#ifdef COMPILER2
      if (FLAG_IS_DEFAULT(MaxLoopPad)) {
        // For new ZX cpus do the next optimization:
        // don't align the beginning of a loop if there are enough instructions
        // left (NumberOfLoopInstrToAlign defined in c2_globals.hpp)
        // in current fetch line (OptoLoopAlignment) or the padding
        // is big (> MaxLoopPad).
        // Set MaxLoopPad to 11 for new ZX cpus to reduce number of
        // generated NOP instructions. 11 is the largest size of one
        // address NOP instruction '0F 1F' (see Assembler::nop(i)).
        MaxLoopPad = 11;
      }
#endif // COMPILER2
      if (FLAG_IS_DEFAULT(UseXMMForArrayCopy)) {
        UseXMMForArrayCopy = true; // use SSE2 movq on new ZX cpus
      }
      if (supports_sse4_2()) { // new ZX cpus
        if (FLAG_IS_DEFAULT(UseUnalignedLoadStores)) {
          UseUnalignedLoadStores = true; // use movdqu on newest ZX cpus
        }
      }
      if (supports_sse4_2()) {
        if (FLAG_IS_DEFAULT(UseSSE42Intrinsics)) {
          FLAG_SET_DEFAULT(UseSSE42Intrinsics, true);
        }
      } else {
        if (UseSSE42Intrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
          warning("SSE4.2 intrinsics require SSE4.2 instructions or higher. Intrinsics will be disabled.");
        }
        FLAG_SET_DEFAULT(UseSSE42Intrinsics, false);
      }
    }

    if (FLAG_IS_DEFAULT(AllocatePrefetchInstr) && supports_3dnow_prefetch()) {
      FLAG_SET_DEFAULT(AllocatePrefetchInstr, 3);
    }
  }

  if (is_amd_family()) { // AMD cpus specific settings
    if (supports_sse2() && FLAG_IS_DEFAULT(UseAddressNop)) {
      // Use it on new AMD cpus starting from Opteron.
      UseAddressNop = true;
    }
    if (supports_sse2() && FLAG_IS_DEFAULT(UseNewLongLShift)) {
      // Use it on new AMD cpus starting from Opteron.
      UseNewLongLShift = true;
    }
    if (FLAG_IS_DEFAULT(UseXmmLoadAndClearUpper)) {
      if (supports_sse4a()) {
        UseXmmLoadAndClearUpper = true; // use movsd only on '10h' Opteron
      } else {
        UseXmmLoadAndClearUpper = false;
      }
    }
    if (FLAG_IS_DEFAULT(UseXmmRegToRegMoveAll)) {
      if (supports_sse4a()) {
        UseXmmRegToRegMoveAll = true; // use movaps, movapd only on '10h'
      } else {
        UseXmmRegToRegMoveAll = false;
      }
    }
    if (FLAG_IS_DEFAULT(UseXmmI2F)) {
      if (supports_sse4a()) {
        UseXmmI2F = true;
      } else {
        UseXmmI2F = false;
      }
    }
    if (FLAG_IS_DEFAULT(UseXmmI2D)) {
      if (supports_sse4a()) {
        UseXmmI2D = true;
      } else {
        UseXmmI2D = false;
      }
    }
    if (supports_sse4_2()) {
      if (FLAG_IS_DEFAULT(UseSSE42Intrinsics)) {
        FLAG_SET_DEFAULT(UseSSE42Intrinsics, true);
      }
    } else {
      if (UseSSE42Intrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
        warning("SSE4.2 intrinsics require SSE4.2 instructions or higher. Intrinsics will be disabled.");
      }
      FLAG_SET_DEFAULT(UseSSE42Intrinsics, false);
    }

    // some defaults for AMD family 15h
    if (cpu_family() == 0x15) {
      // On family 15h processors default is no sw prefetch
      if (FLAG_IS_DEFAULT(AllocatePrefetchStyle)) {
        FLAG_SET_DEFAULT(AllocatePrefetchStyle, 0);
      }
      // Also, if some other prefetch style is specified, default instruction type is PREFETCHW
      if (FLAG_IS_DEFAULT(AllocatePrefetchInstr)) {
        FLAG_SET_DEFAULT(AllocatePrefetchInstr, 3);
      }
      // On family 15h processors use XMM and UnalignedLoadStores for Array Copy
      if (supports_sse2() && FLAG_IS_DEFAULT(UseXMMForArrayCopy)) {
        FLAG_SET_DEFAULT(UseXMMForArrayCopy, true);
      }
      if (supports_sse2() && FLAG_IS_DEFAULT(UseUnalignedLoadStores)) {
        FLAG_SET_DEFAULT(UseUnalignedLoadStores, true);
      }
    }

#ifdef COMPILER2
    if (cpu_family() < 0x17 && MaxVectorSize > 16) {
      // Limit vectors size to 16 bytes on AMD cpus < 17h.
      FLAG_SET_DEFAULT(MaxVectorSize, 16);
    }
#endif // COMPILER2

    // Some defaults for AMD family >= 17h && Hygon family 18h
    if (cpu_family() >= 0x17) {
      // On family >=17h processors use XMM and UnalignedLoadStores
      // for Array Copy
      if (supports_sse2() && FLAG_IS_DEFAULT(UseXMMForArrayCopy)) {
        FLAG_SET_DEFAULT(UseXMMForArrayCopy, true);
      }
      if (supports_sse2() && FLAG_IS_DEFAULT(UseUnalignedLoadStores)) {
        FLAG_SET_DEFAULT(UseUnalignedLoadStores, true);
      }
#ifdef COMPILER2
      if (supports_sse4_2() && FLAG_IS_DEFAULT(UseFPUForSpilling)) {
        FLAG_SET_DEFAULT(UseFPUForSpilling, true);
      }
#endif
    }
  }

  if (is_intel()) { // Intel cpus specific settings
    if (FLAG_IS_DEFAULT(UseStoreImmI16)) {
      UseStoreImmI16 = false; // don't use it on Intel cpus
    }
    if (cpu_family() == 6 || cpu_family() == 15) {
      if (FLAG_IS_DEFAULT(UseAddressNop)) {
        // Use it on all Intel cpus starting from PentiumPro
        UseAddressNop = true;
      }
    }
    if (FLAG_IS_DEFAULT(UseXmmLoadAndClearUpper)) {
      UseXmmLoadAndClearUpper = true; // use movsd on all Intel cpus
    }
    if (FLAG_IS_DEFAULT(UseXmmRegToRegMoveAll)) {
      if (supports_sse3()) {
        UseXmmRegToRegMoveAll = true; // use movaps, movapd on new Intel cpus
      } else {
        UseXmmRegToRegMoveAll = false;
      }
    }
    if (cpu_family() == 6 && supports_sse3()) { // New Intel cpus
#ifdef COMPILER2
      if (FLAG_IS_DEFAULT(MaxLoopPad)) {
        // For new Intel cpus do the next optimization:
        // don't align the beginning of a loop if there are enough instructions
        // left (NumberOfLoopInstrToAlign defined in c2_globals.hpp)
        // in current fetch line (OptoLoopAlignment) or the padding
        // is big (> MaxLoopPad).
        // Set MaxLoopPad to 11 for new Intel cpus to reduce number of
        // generated NOP instructions. 11 is the largest size of one
        // address NOP instruction '0F 1F' (see Assembler::nop(i)).
        MaxLoopPad = 11;
      }
#endif // COMPILER2

      if (FLAG_IS_DEFAULT(UseXMMForArrayCopy)) {
        UseXMMForArrayCopy = true; // use SSE2 movq on new Intel cpus
      }
      if ((supports_sse4_2() && supports_ht()) || supports_avx()) { // Newest Intel cpus
        if (FLAG_IS_DEFAULT(UseUnalignedLoadStores)) {
          UseUnalignedLoadStores = true; // use movdqu on newest Intel cpus
        }
      }
      if (supports_sse4_2()) {
        if (FLAG_IS_DEFAULT(UseSSE42Intrinsics)) {
          FLAG_SET_DEFAULT(UseSSE42Intrinsics, true);
        }
      } else {
        if (UseSSE42Intrinsics && !FLAG_IS_DEFAULT(UseAESIntrinsics)) {
          warning("SSE4.2 intrinsics require SSE4.2 instructions or higher. Intrinsics will be disabled.");
        }
        FLAG_SET_DEFAULT(UseSSE42Intrinsics, false);
      }
    }
    if (is_atom_family() || is_knights_family()) {
#ifdef COMPILER2
      if (FLAG_IS_DEFAULT(OptoScheduling)) {
        OptoScheduling = true;
      }
#endif
      if (supports_sse4_2()) { // Silvermont
        if (FLAG_IS_DEFAULT(UseUnalignedLoadStores)) {
          UseUnalignedLoadStores = true; // use movdqu on newest Intel cpus
        }
      }
      if (FLAG_IS_DEFAULT(UseIncDec)) {
        FLAG_SET_DEFAULT(UseIncDec, false);
      }
    }
    if (FLAG_IS_DEFAULT(AllocatePrefetchInstr) && supports_3dnow_prefetch()) {
      FLAG_SET_DEFAULT(AllocatePrefetchInstr, 3);
    }
#ifdef COMPILER2
    if (UseAVX > 2) {
      if (FLAG_IS_DEFAULT(ArrayOperationPartialInlineSize) ||
          (!FLAG_IS_DEFAULT(ArrayOperationPartialInlineSize) &&
           ArrayOperationPartialInlineSize != 0 &&
           ArrayOperationPartialInlineSize != 16 &&
           ArrayOperationPartialInlineSize != 32 &&
           ArrayOperationPartialInlineSize != 64)) {
        int inline_size = 0;
        if (MaxVectorSize >= 64 && AVX3Threshold == 0) {
          inline_size = 64;
        } else if (MaxVectorSize >= 32) {
          inline_size = 32;
        } else if (MaxVectorSize >= 16) {
          inline_size = 16;
        }
        if(!FLAG_IS_DEFAULT(ArrayOperationPartialInlineSize)) {
          warning("Setting ArrayOperationPartialInlineSize as %d", inline_size);
        }
        ArrayOperationPartialInlineSize = inline_size;
      }

      if (ArrayOperationPartialInlineSize > MaxVectorSize) {
        ArrayOperationPartialInlineSize = MaxVectorSize >= 16 ? MaxVectorSize : 0;
        if (ArrayOperationPartialInlineSize) {
          warning("Setting ArrayOperationPartialInlineSize as MaxVectorSize" INTX_FORMAT ")", MaxVectorSize);
        } else {
          warning("Setting ArrayOperationPartialInlineSize as " INTX_FORMAT, ArrayOperationPartialInlineSize);
        }
      }
    }
#endif
  }

#ifdef _LP64
  if (UseSSE42Intrinsics) {
    if (FLAG_IS_DEFAULT(UseVectorizedMismatchIntrinsic)) {
      UseVectorizedMismatchIntrinsic = true;
    }
  } else if (UseVectorizedMismatchIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseVectorizedMismatchIntrinsic))
      warning("vectorizedMismatch intrinsics are not available on this CPU");
    FLAG_SET_DEFAULT(UseVectorizedMismatchIntrinsic, false);
  }
#else
  if (UseVectorizedMismatchIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseVectorizedMismatchIntrinsic)) {
      warning("vectorizedMismatch intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseVectorizedMismatchIntrinsic, false);
  }
#endif // _LP64

  // Use count leading zeros count instruction if available.
  if (supports_lzcnt()) {
    if (FLAG_IS_DEFAULT(UseCountLeadingZerosInstruction)) {
      UseCountLeadingZerosInstruction = true;
    }
   } else if (UseCountLeadingZerosInstruction) {
    warning("lzcnt instruction is not available on this CPU");
    FLAG_SET_DEFAULT(UseCountLeadingZerosInstruction, false);
  }

  // Use count trailing zeros instruction if available
  if (supports_bmi1()) {
    // tzcnt does not require VEX prefix
    if (FLAG_IS_DEFAULT(UseCountTrailingZerosInstruction)) {
      if (!UseBMI1Instructions && !FLAG_IS_DEFAULT(UseBMI1Instructions)) {
        // Don't use tzcnt if BMI1 is switched off on command line.
        UseCountTrailingZerosInstruction = false;
      } else {
        UseCountTrailingZerosInstruction = true;
      }
    }
  } else if (UseCountTrailingZerosInstruction) {
    warning("tzcnt instruction is not available on this CPU");
    FLAG_SET_DEFAULT(UseCountTrailingZerosInstruction, false);
  }

  // BMI instructions (except tzcnt) use an encoding with VEX prefix.
  // VEX prefix is generated only when AVX > 0.
  if (supports_bmi1() && supports_avx()) {
    if (FLAG_IS_DEFAULT(UseBMI1Instructions)) {
      UseBMI1Instructions = true;
    }
  } else if (UseBMI1Instructions) {
    warning("BMI1 instructions are not available on this CPU (AVX is also required)");
    FLAG_SET_DEFAULT(UseBMI1Instructions, false);
  }

  if (supports_bmi2() && supports_avx()) {
    if (FLAG_IS_DEFAULT(UseBMI2Instructions)) {
      UseBMI2Instructions = true;
    }
  } else if (UseBMI2Instructions) {
    warning("BMI2 instructions are not available on this CPU (AVX is also required)");
    FLAG_SET_DEFAULT(UseBMI2Instructions, false);
  }

  // Use population count instruction if available.
  if (supports_popcnt()) {
    if (FLAG_IS_DEFAULT(UsePopCountInstruction)) {
      UsePopCountInstruction = true;
    }
  } else if (UsePopCountInstruction) {
    warning("POPCNT instruction is not available on this CPU");
    FLAG_SET_DEFAULT(UsePopCountInstruction, false);
  }

  // Use fast-string operations if available.
  if (supports_erms()) {
    if (FLAG_IS_DEFAULT(UseFastStosb)) {
      UseFastStosb = true;
    }
  } else if (UseFastStosb) {
    warning("fast-string operations are not available on this CPU");
    FLAG_SET_DEFAULT(UseFastStosb, false);
  }

  // For AMD Processors use XMM/YMM MOVDQU instructions
  // for Object Initialization as default
  if (is_amd() && cpu_family() >= 0x19) {
    if (FLAG_IS_DEFAULT(UseFastStosb)) {
      UseFastStosb = false;
    }
  }

#ifdef COMPILER2
  if (is_intel() && MaxVectorSize > 16) {
    if (FLAG_IS_DEFAULT(UseFastStosb)) {
      UseFastStosb = false;
    }
  }
#endif

  // Use XMM/YMM MOVDQU instruction for Object Initialization
  if (!UseFastStosb && UseSSE >= 2 && UseUnalignedLoadStores) {
    if (FLAG_IS_DEFAULT(UseXMMForObjInit)) {
      UseXMMForObjInit = true;
    }
  } else if (UseXMMForObjInit) {
    warning("UseXMMForObjInit requires SSE2 and unaligned load/stores. Feature is switched off.");
    FLAG_SET_DEFAULT(UseXMMForObjInit, false);
  }

#ifdef COMPILER2
  if (FLAG_IS_DEFAULT(AlignVector)) {
    // Modern processors allow misaligned memory operations for vectors.
    AlignVector = !UseUnalignedLoadStores;
  }
  if (FLAG_IS_DEFAULT(OptimizeFill)) {
    // 8247307: On x86, the auto-vectorized loop array fill code shows
    // better performance than the array fill stubs. We should reenable
    // this after the x86 stubs get improved.
    OptimizeFill = false;
  }
#endif // COMPILER2

  if (FLAG_IS_DEFAULT(AllocatePrefetchInstr)) {
    if (AllocatePrefetchInstr == 3 && !supports_3dnow_prefetch()) {
      FLAG_SET_DEFAULT(AllocatePrefetchInstr, 0);
    } else if (!supports_sse() && supports_3dnow_prefetch()) {
      FLAG_SET_DEFAULT(AllocatePrefetchInstr, 3);
    }
  }

  // Allocation prefetch settings
  intx cache_line_size = prefetch_data_size();
  if (FLAG_IS_DEFAULT(AllocatePrefetchStepSize) &&
      (cache_line_size > AllocatePrefetchStepSize)) {
    FLAG_SET_DEFAULT(AllocatePrefetchStepSize, cache_line_size);
  }

  if ((AllocatePrefetchDistance == 0) && (AllocatePrefetchStyle != 0)) {
    assert(!FLAG_IS_DEFAULT(AllocatePrefetchDistance), "default value should not be 0");
    if (!FLAG_IS_DEFAULT(AllocatePrefetchStyle)) {
      warning("AllocatePrefetchDistance is set to 0 which disable prefetching. Ignoring AllocatePrefetchStyle flag.");
    }
    FLAG_SET_DEFAULT(AllocatePrefetchStyle, 0);
  }

  if (FLAG_IS_DEFAULT(AllocatePrefetchDistance)) {
    bool use_watermark_prefetch = (AllocatePrefetchStyle == 2);
    FLAG_SET_DEFAULT(AllocatePrefetchDistance, allocate_prefetch_distance(use_watermark_prefetch));
  }

  if (is_intel() && cpu_family() == 6 && supports_sse3()) {
    if (FLAG_IS_DEFAULT(AllocatePrefetchLines) &&
        supports_sse4_2() && supports_ht()) { // Nehalem based cpus
      FLAG_SET_DEFAULT(AllocatePrefetchLines, 4);
    }
#ifdef COMPILER2
    if (FLAG_IS_DEFAULT(UseFPUForSpilling) && supports_sse4_2()) {
      FLAG_SET_DEFAULT(UseFPUForSpilling, true);
    }
#endif
  }

  if (is_zx() && ((cpu_family() == 6) || (cpu_family() == 7)) && supports_sse4_2()) {
#ifdef COMPILER2
    if (FLAG_IS_DEFAULT(UseFPUForSpilling)) {
      FLAG_SET_DEFAULT(UseFPUForSpilling, true);
    }
#endif
  }

#ifdef _LP64
  // Prefetch settings

  // Prefetch interval for gc copy/scan == 9 dcache lines.  Derived from
  // 50-warehouse specjbb runs on a 2-way 1.8ghz opteron using a 4gb heap.
  // Tested intervals from 128 to 2048 in increments of 64 == one cache line.
  // 256 bytes (4 dcache lines) was the nearest runner-up to 576.

  // gc copy/scan is disabled if prefetchw isn't supported, because
  // Prefetch::write emits an inlined prefetchw on Linux.
  // Do not use the 3dnow prefetchw instruction.  It isn't supported on em64t.
  // The used prefetcht0 instruction works for both amd64 and em64t.

  if (FLAG_IS_DEFAULT(PrefetchCopyIntervalInBytes)) {
    FLAG_SET_DEFAULT(PrefetchCopyIntervalInBytes, 576);
  }
  if (FLAG_IS_DEFAULT(PrefetchScanIntervalInBytes)) {
    FLAG_SET_DEFAULT(PrefetchScanIntervalInBytes, 576);
  }
  if (FLAG_IS_DEFAULT(PrefetchFieldsAhead)) {
    FLAG_SET_DEFAULT(PrefetchFieldsAhead, 1);
  }
#endif

  if (FLAG_IS_DEFAULT(ContendedPaddingWidth) &&
     (cache_line_size > ContendedPaddingWidth))
     ContendedPaddingWidth = cache_line_size;

  // This machine allows unaligned memory accesses
  if (FLAG_IS_DEFAULT(UseUnalignedAccesses)) {
    FLAG_SET_DEFAULT(UseUnalignedAccesses, true);
  }

#ifndef PRODUCT
  if (log_is_enabled(Info, os, cpu)) {
    LogStream ls(Log(os, cpu)::info());
    outputStream* log = &ls;
    log->print_cr("Logical CPUs per core: %u",
                  logical_processors_per_package());
    log->print_cr("L1 data cache line size: %u", L1_data_cache_line_size());
    log->print("UseSSE=%d", (int) UseSSE);
    if (UseAVX > 0) {
      log->print("  UseAVX=%d", (int) UseAVX);
    }
    if (UseAES) {
      log->print("  UseAES=1");
    }
#ifdef COMPILER2
    if (MaxVectorSize > 0) {
      log->print("  MaxVectorSize=%d", (int) MaxVectorSize);
    }
#endif
    log->cr();
    log->print("Allocation");
    if (AllocatePrefetchStyle <= 0 || (UseSSE == 0 && !supports_3dnow_prefetch())) {
      log->print_cr(": no prefetching");
    } else {
      log->print(" prefetching: ");
      if (UseSSE == 0 && supports_3dnow_prefetch()) {
        log->print("PREFETCHW");
      } else if (UseSSE >= 1) {
        if (AllocatePrefetchInstr == 0) {
          log->print("PREFETCHNTA");
        } else if (AllocatePrefetchInstr == 1) {
          log->print("PREFETCHT0");
        } else if (AllocatePrefetchInstr == 2) {
          log->print("PREFETCHT2");
        } else if (AllocatePrefetchInstr == 3) {
          log->print("PREFETCHW");
        }
      }
      if (AllocatePrefetchLines > 1) {
        log->print_cr(" at distance %d, %d lines of %d bytes", (int) AllocatePrefetchDistance, (int) AllocatePrefetchLines, (int) AllocatePrefetchStepSize);
      } else {
        log->print_cr(" at distance %d, one line of %d bytes", (int) AllocatePrefetchDistance, (int) AllocatePrefetchStepSize);
      }
    }

    if (PrefetchCopyIntervalInBytes > 0) {
      log->print_cr("PrefetchCopyIntervalInBytes %d", (int) PrefetchCopyIntervalInBytes);
    }
    if (PrefetchScanIntervalInBytes > 0) {
      log->print_cr("PrefetchScanIntervalInBytes %d", (int) PrefetchScanIntervalInBytes);
    }
    if (PrefetchFieldsAhead > 0) {
      log->print_cr("PrefetchFieldsAhead %d", (int) PrefetchFieldsAhead);
    }
    if (ContendedPaddingWidth > 0) {
      log->print_cr("ContendedPaddingWidth %d", (int) ContendedPaddingWidth);
    }
  }
#endif // !PRODUCT
  if (FLAG_IS_DEFAULT(UseSignumIntrinsic)) {
      FLAG_SET_DEFAULT(UseSignumIntrinsic, true);
  }
}

void VM_Version::print_platform_virtualization_info(outputStream* st) {
  VirtualizationType vrt = VM_Version::get_detected_virtualization();
  if (vrt == XenHVM) {
    st->print_cr("Xen hardware-assisted virtualization detected");
  } else if (vrt == KVM) {
    st->print_cr("KVM virtualization detected");
  } else if (vrt == VMWare) {
    st->print_cr("VMWare virtualization detected");
    VirtualizationSupport::print_virtualization_info(st);
  } else if (vrt == HyperV) {
    st->print_cr("Hyper-V virtualization detected");
  } else if (vrt == HyperVRole) {
    st->print_cr("Hyper-V role detected");
  }
}

bool VM_Version::use_biased_locking() {
#if INCLUDE_RTM_OPT
  // RTM locking is most useful when there is high lock contention and
  // low data contention.  With high lock contention the lock is usually
  // inflated and biased locking is not suitable for that case.
  // RTM locking code requires that biased locking is off.
  // Note: we can't switch off UseBiasedLocking in get_processor_features()
  // because it is used by Thread::allocate() which is called before
  // VM_Version::initialize().
  if (UseRTMLocking && UseBiasedLocking) {
    if (FLAG_IS_DEFAULT(UseBiasedLocking)) {
      FLAG_SET_DEFAULT(UseBiasedLocking, false);
    } else {
      warning("Biased locking is not supported with RTM locking; ignoring UseBiasedLocking flag." );
      UseBiasedLocking = false;
    }
  }
#endif
  return UseBiasedLocking;
}

bool VM_Version::compute_has_intel_jcc_erratum() {
  if (!is_intel_family_core()) {
    // Only Intel CPUs are affected.
    return false;
  }
  // The following table of affected CPUs is based on the following document released by Intel:
  // https://www.intel.com/content/dam/support/us/en/documents/processors/mitigations-jump-conditional-code-erratum.pdf
  switch (_model) {
  case 0x8E:
    // 06_8EH | 9 | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Amber Lake Y
    // 06_8EH | 9 | 7th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake U
    // 06_8EH | 9 | 7th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake U 23e
    // 06_8EH | 9 | 7th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake Y
    // 06_8EH | A | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake U43e
    // 06_8EH | B | 8th Generation Intel® Core™ Processors based on microarchitecture code name Whiskey Lake U
    // 06_8EH | C | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Amber Lake Y
    // 06_8EH | C | 10th Generation Intel® Core™ Processor Family based on microarchitecture code name Comet Lake U42
    // 06_8EH | C | 8th Generation Intel® Core™ Processors based on microarchitecture code name Whiskey Lake U
    return _stepping == 0x9 || _stepping == 0xA || _stepping == 0xB || _stepping == 0xC;
  case 0x4E:
    // 06_4E  | 3 | 6th Generation Intel® Core™ Processors based on microarchitecture code name Skylake U
    // 06_4E  | 3 | 6th Generation Intel® Core™ Processor Family based on microarchitecture code name Skylake U23e
    // 06_4E  | 3 | 6th Generation Intel® Core™ Processors based on microarchitecture code name Skylake Y
    return _stepping == 0x3;
  case 0x55:
    // 06_55H | 4 | Intel® Xeon® Processor D Family based on microarchitecture code name Skylake D, Bakerville
    // 06_55H | 4 | Intel® Xeon® Scalable Processors based on microarchitecture code name Skylake Server
    // 06_55H | 4 | Intel® Xeon® Processor W Family based on microarchitecture code name Skylake W
    // 06_55H | 4 | Intel® Core™ X-series Processors based on microarchitecture code name Skylake X
    // 06_55H | 4 | Intel® Xeon® Processor E3 v5 Family based on microarchitecture code name Skylake Xeon E3
    // 06_55  | 7 | 2nd Generation Intel® Xeon® Scalable Processors based on microarchitecture code name Cascade Lake (server)
    return _stepping == 0x4 || _stepping == 0x7;
  case 0x5E:
    // 06_5E  | 3 | 6th Generation Intel® Core™ Processor Family based on microarchitecture code name Skylake H
    // 06_5E  | 3 | 6th Generation Intel® Core™ Processor Family based on microarchitecture code name Skylake S
    return _stepping == 0x3;
  case 0x9E:
    // 06_9EH | 9 | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake G
    // 06_9EH | 9 | 7th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake H
    // 06_9EH | 9 | 7th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake S
    // 06_9EH | 9 | Intel® Core™ X-series Processors based on microarchitecture code name Kaby Lake X
    // 06_9EH | 9 | Intel® Xeon® Processor E3 v6 Family Kaby Lake Xeon E3
    // 06_9EH | A | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake H
    // 06_9EH | A | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake S
    // 06_9EH | A | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake S (6+2) x/KBP
    // 06_9EH | A | Intel® Xeon® Processor E Family based on microarchitecture code name Coffee Lake S (6+2)
    // 06_9EH | A | Intel® Xeon® Processor E Family based on microarchitecture code name Coffee Lake S (4+2)
    // 06_9EH | B | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake S (4+2)
    // 06_9EH | B | Intel® Celeron® Processor G Series based on microarchitecture code name Coffee Lake S (4+2)
    // 06_9EH | D | 9th Generation Intel® Core™ Processor Family based on microarchitecturecode name Coffee Lake H (8+2)
    // 06_9EH | D | 9th Generation Intel® Core™ Processor Family based on microarchitecture code name Coffee Lake S (8+2)
    return _stepping == 0x9 || _stepping == 0xA || _stepping == 0xB || _stepping == 0xD;
  case 0xA5:
    // Not in Intel documentation.
    // 06_A5H |    | 10th Generation Intel® Core™ Processor Family based on microarchitecture code name Comet Lake S/H
    return true;
  case 0xA6:
    // 06_A6H | 0  | 10th Generation Intel® Core™ Processor Family based on microarchitecture code name Comet Lake U62
    return _stepping == 0x0;
  case 0xAE:
    // 06_AEH | A | 8th Generation Intel® Core™ Processor Family based on microarchitecture code name Kaby Lake Refresh U (4+2)
    return _stepping == 0xA;
  default:
    // If we are running on another intel machine not recognized in the table, we are okay.
    return false;
  }
}

// On Xen, the cpuid instruction returns
//  eax / registers[0]: Version of Xen
//  ebx / registers[1]: chars 'XenV'
//  ecx / registers[2]: chars 'MMXe'
//  edx / registers[3]: chars 'nVMM'
//
// On KVM / VMWare / MS Hyper-V, the cpuid instruction returns
//  ebx / registers[1]: chars 'KVMK' / 'VMwa' / 'Micr'
//  ecx / registers[2]: chars 'VMKV' / 'reVM' / 'osof'
//  edx / registers[3]: chars 'M'    / 'ware' / 't Hv'
//
// more information :
// https://kb.vmware.com/s/article/1009458
//
void VM_Version::check_virtualizations() {
  uint32_t registers[4] = {0};
  char signature[13] = {0};

  // Xen cpuid leaves can be found 0x100 aligned boundary starting
  // from 0x40000000 until 0x40010000.
  //   https://lists.linuxfoundation.org/pipermail/virtualization/2012-May/019974.html
  for (int leaf = 0x40000000; leaf < 0x40010000; leaf += 0x100) {
    detect_virt_stub(leaf, registers);
    memcpy(signature, &registers[1], 12);

    if (strncmp("VMwareVMware", signature, 12) == 0) {
      Abstract_VM_Version::_detected_virtualization = VMWare;
      // check for extended metrics from guestlib
      VirtualizationSupport::initialize();
    } else if (strncmp("Microsoft Hv", signature, 12) == 0) {
      Abstract_VM_Version::_detected_virtualization = HyperV;
#ifdef _WINDOWS
      // CPUID leaf 0x40000007 is available to the root partition only.
      // See Hypervisor Top Level Functional Specification section 2.4.8 for more details.
      //   https://github.com/MicrosoftDocs/Virtualization-Documentation/raw/master/tlfs/Hypervisor%20Top%20Level%20Functional%20Specification%20v6.0b.pdf
      detect_virt_stub(0x40000007, registers);
      if ((registers[0] != 0x0) ||
          (registers[1] != 0x0) ||
          (registers[2] != 0x0) ||
          (registers[3] != 0x0)) {
        Abstract_VM_Version::_detected_virtualization = HyperVRole;
      }
#endif
    } else if (strncmp("KVMKVMKVM", signature, 9) == 0) {
      Abstract_VM_Version::_detected_virtualization = KVM;
    } else if (strncmp("XenVMMXenVMM", signature, 12) == 0) {
      Abstract_VM_Version::_detected_virtualization = XenHVM;
    }
  }
}

// Print the feature names as " = feat1, ..., featN\n";
void VM_Version::fatal_missing_features(uint64_t features_missing, uint64_t glibc_features_missing) {
  static const char part1[] = "; missing features of this CPU are ";
  tty->print_raw(part1, sizeof(part1) - 1);
  nonlibc_tty_print_uint64_comma_uint64(features_missing, glibc_features_missing);
  static const char part2[] = " =";
  tty->print_raw(part2, sizeof(part2) - 1);
  char buf[512] = "";
  // insert_features_names() does crash for undefined too high-numbered features.
  insert_features_names(buf, sizeof(buf)          ,       features_missing & (  MAX_CPU - 1));
  char *s = buf;
  while (*s)
    ++s;
  insert_features_names(s  , buf + sizeof(buf) - s, glibc_features_missing & (MAX_GLIBC - 1));
  while (*s)
    ++s;
  /* +1 to skip the first ','. */
  tty->print_raw(buf + 1, s - (buf + 1));
  tty->cr();
  vm_exit_during_initialization();
}

void VM_Version::crac_restore() {
  assert(CRaCCheckpointTo != NULL, "");

  if (ShowCPUFeatures) {
    static const char prefix[] = "This snapshot's stored CPU features are: -XX:CPUFeatures=";
    tty->print_raw(prefix, sizeof(prefix) - 1);
    nonlibc_tty_print_uint64_comma_uint64(_features, _glibc_features);
    tty->cr();
  }

  uint64_t       features_saved =       _features;
  uint64_t glibc_features_saved = _glibc_features;
#define SUPPORTS_SET \
    SUPPORTS(supports_cx8) \
    SUPPORTS(supports_atomic_getset4) \
    SUPPORTS(supports_atomic_getset8) \
    SUPPORTS(supports_atomic_getadd4) \
    SUPPORTS(supports_atomic_getadd8) \
    /**/
#define SUPPORTS(x) bool x##_saved = Abstract_VM_Version::x();
  SUPPORTS_SET
#undef SUPPORTS

  get_processor_features_hardware();

  uint64_t       features_missing =       features_saved & ~      _features;
  uint64_t glibc_features_missing = glibc_features_saved & ~_glibc_features;

  // Workaround JDK-8311164: CPU_HT is set randomly on hybrid CPUs like Alder Lake.
  features_missing &= ~CPU_HT;

  if (features_missing || glibc_features_missing) {
    static const char part1[] = "You have to specify -XX:CPUFeatures=";
    tty->print_raw(part1, sizeof(part1) - 1);
    nonlibc_tty_print_uint64_comma_uint64(_features & features_saved, _glibc_features & glibc_features_saved);
    static const char part2[] = " together with -XX:CRaCCheckpointTo when making a checkpoint file; specified -XX:CRaCRestoreFrom file contains CPU features ";
    tty->print_raw(part2, sizeof(part2) - 1);
    nonlibc_tty_print_uint64_comma_uint64(features_saved, glibc_features_saved);
    fatal_missing_features(features_missing, glibc_features_missing);
  }

  auto supports_exit = [&](const char *supports, bool file, bool this_cpu) {
    char buf[512];
    int res = jio_snprintf(
                buf, sizeof(buf),
                "Specified -XX:CRaCRestoreFrom file contains feature \"%s\" value %d while this CPU has value %d",
                supports, file, this_cpu);
    assert(res > 0, "not enough temporary space allocated");
    vm_exit_during_initialization(buf);
  };
#define SUPPORTS(x)                                           \
  if (x##_saved != Abstract_VM_Version::x()) {                \
    supports_exit( #x , Abstract_VM_Version::x(), x##_saved); \
  }
  SUPPORTS_SET
#undef SUPPORTS
#undef SUPPORTS_SET

  if (ShowCPUFeatures)
    print_using_features_cr();
}

void VM_Version::initialize() {
  ResourceMark rm;
  // Making this stub must be FIRST use of assembler
  stub_blob = BufferBlob::create("VM_Version stub", stub_size);
  if (stub_blob == NULL) {
    vm_exit_during_initialization("Unable to allocate stub for VM_Version");
  }
  CodeBuffer c(stub_blob);
  VM_Version_StubGenerator g(&c);

  get_cpu_info_stub = CAST_TO_FN_PTR(get_cpu_info_stub_t,
                                     g.generate_get_cpu_info());
  detect_virt_stub = CAST_TO_FN_PTR(detect_virt_stub_t,
                                     g.generate_detect_virt());

  assert(      _features == 0,       "_features should be zero at startup");
  assert(_glibc_features == 0, "_glibc_features should be zero at startup");
  get_processor_features_hardware();

  assert(!CPUFeatures == FLAG_IS_DEFAULT(CPUFeatures), "CPUFeatures parsing");
  uint64_t GLIBCFeatures_x64;
  uint64_t   CPUFeatures_x64 = CPUFeatures_parse(GLIBCFeatures_x64);
  uint64_t       features_missing =   CPUFeatures_x64 & ~      _features;
  uint64_t glibc_features_missing = GLIBCFeatures_x64 & ~_glibc_features;

  // Workaround JDK-8311164: CPU_HT is set randomly on hybrid CPUs like Alder Lake.
  features_missing &= ~CPU_HT;

  if (features_missing || glibc_features_missing) {
    static const char part1[] = "Specified -XX:CPUFeatures=";
    tty->print_raw(part1, sizeof(part1) - 1);
    nonlibc_tty_print_uint64_comma_uint64(CPUFeatures_x64, GLIBCFeatures_x64);
    static const char part2[] = "; this machine's CPU features are ";
    tty->print_raw(part2, sizeof(part2) - 1);
    nonlibc_tty_print_uint64_comma_uint64(_features, _glibc_features);
    fatal_missing_features(features_missing, glibc_features_missing);
  }

  uint64_t       features_saved =       _features;
  uint64_t glibc_features_saved = _glibc_features;

        _features =   CPUFeatures_x64;
  _glibc_features = GLIBCFeatures_x64;

  if (ShowCPUFeatures)
    print_using_features_cr();

#ifdef LINUX
  if (!ignore_glibc_not_using) {
    uint64_t       features_expected =   MAX_CPU - 1;
    uint64_t glibc_features_expected = MAX_GLIBC - 1;
#if !INCLUDE_CPU_FEATURE_ACTIVE && !INCLUDE_LD_SO_LIST_DIAGNOSTICS
          features_expected =       features_saved;
    glibc_features_expected = glibc_features_saved;
#endif
    glibc_not_using(      features_expected & ~      _features,
                    glibc_features_expected & ~_glibc_features);
  }
#endif

  get_processor_features_hotspot();

  LP64_ONLY(Assembler::precompute_instructions();)

  if (VM_Version::supports_hv()) { // Supports hypervisor
    check_virtualizations();
  }
}
