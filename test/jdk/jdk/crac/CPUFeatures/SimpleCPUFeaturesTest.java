/*
 * Copyright (c) 2023, 2025, Azul Systems, Inc. All rights reserved.
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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.test.lib.crac.*;
import jdk.test.lib.process.OutputAnalyzer;

/*
 * @test id=NAMED
 * @library /test/lib
 * @build SimpleCPUFeaturesTest
 * @run driver jdk.test.lib.crac.CracTest native
 * @run driver jdk.test.lib.crac.CracTest generic
 * @run driver jdk.test.lib.crac.CracTest ignore
 */
/*
 * @test id=TUNABLES
 * @requires os.family == "linux"
 * @requires os.arch=="amd64" | os.arch=="x86_64"
 * @library /test/lib
 * @build SimpleCPUFeaturesTest
 * @summary On old glibc (without INCLUDE_CPU_FEATURE_ACTIVE) one could get an internal error.
 * @comment   Error occurred during initialization of VM
 * @comment   internal error: GLIBC_TUNABLES=:glibc.cpu.hwcaps=,-AVX,-FMA,-AVX2,-BMI1,-BMI2,-ERMS,-LZCNT,-SSSE3,-POPCNT,-SSE4_1,-SSE4_2,-AVX512F,-AVX512CD,-AVX512BW,-AVX512DQ,-AVX512VL,-IBT,-MOVBE,-SHSTK,-XSAVE,-OSXSAVE,-HTT failed and HOTSPOT_GLIBC_TUNABLES_REEXEC is set
 * @run driver jdk.test.lib.crac.CracTest generic glibc.pthread.rseq=0
 * @run driver jdk.test.lib.crac.CracTest generic                      glibc.cpu.hwcaps=-AVX
 * @run driver jdk.test.lib.crac.CracTest generic glibc.pthread.rseq=0:glibc.cpu.hwcaps=-AVX
 * @run driver jdk.test.lib.crac.CracTest generic                      glibc.cpu.hwcaps=-AVX:glibc.pthread.rseq=0
 */
/*
 * @test id=X86-LINUX
 * @requires os.family == "linux"
 * @requires os.arch=="amd64" | os.arch=="x86_64"
 * @library /test/lib
 * @build SimpleCPUFeaturesTest
 * @comment FLUSH and SSE2 must be present
 * @run driver jdk.test.lib.crac.CracTest 0x20000000080,0x0
 * @run driver jdk.test.lib.crac.CracTest foobar              -- INVALID_FORMAT
 * @run driver jdk.test.lib.crac.CracTest 0xfffff,0x0         -- MISSING_FEATURES
 * @run driver jdk.test.lib.crac.CracTest 0x20000000080,0xfff -- MISSING_FEATURES
 */
/*
 * @test id=x86-NON-LINUX
 * @requires os.arch=="amd64" | os.arch=="x86_64"
 * @requires os.family != "linux"
 * @library /test/lib
 * @build SimpleCPUFeaturesTest
 * @run driver jdk.test.lib.crac.CracTest foobar            -- OS_DOES_NOT_SUPPORT
 * @run driver jdk.test.lib.crac.CracTest 0x20000000080,0x0 -- OS_DOES_NOT_SUPPORT
 */
/*
 * @test id=AARCH64
 * @requires os.arch=="aarch64"
 * @library /test/lib
 * @build SimpleCPUFeaturesTest
 * @run driver jdk.test.lib.crac.CracTest 0x0,0x0 -- ARCH_DOES_NOT_SUPPORT
 * @run driver jdk.test.lib.crac.CracTest foobar  -- ARCH_DOES_NOT_SUPPORT
 */
public class SimpleCPUFeaturesTest implements CracTest {
    private static final String SUCCESS = "SUCCESS";

    // jtreg does not respect quoted strings
    private enum ErrorMsg {
        INVALID_FORMAT("must be of the form: 0xNUM,0xNUM"),
        MISSING_FEATURES("missing features of this CPU are"),
        ARCH_DOES_NOT_SUPPORT("This architecture does not support any arch-specific"),
        OS_DOES_NOT_SUPPORT("This OS does not support"),
        ;
        final String msg;

        ErrorMsg(String msg) {
            this.msg = msg;
        }
    }

    @CracTestArg(0)
    String cpuFeatures;

    @CracTestArg(value = 1, optional = true)
    String glibcTunables;

    @CracTestArg(value = 2, optional = true)
    ErrorMsg error;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder()
                .captureOutput(true)
                .vmOption("-XX:CPUFeatures=" + cpuFeatures)
                .vmOption("-XX:+ShowCPUFeatures");
        if (glibcTunables != null && !"--".equals(glibcTunables)) {
            builder.env("GLIBC_TUNABLES", glibcTunables);
        }
        CracProcess process = builder.startPlain();
        process.waitFor();
        OutputAnalyzer oa = process.outputAnalyzer();
        if (error == null) {
            oa.shouldHaveExitValue(0)
                    .stdoutShouldContain(SUCCESS);
        } else {
            oa.shouldNotHaveExitValue(0)
                    .stdoutShouldContain(error.msg);
        }
    }

    @Override
    public void exec() {
        System.out.println(SUCCESS);
    }
}
