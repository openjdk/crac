/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

/*
 * @test id=HAS_PACA
 * @requires (os.family == "linux")
 * @requires (os.arch == "aarch64" & vm.cpu.features ~= ".*\bpaca\b.*")
 * @library /test/lib
 * @build PointerAuthenticationTest
 * @run driver jdk.test.lib.crac.CracTest     - true  pass
 * @run driver jdk.test.lib.crac.CracTest     - false fail
 * @run driver jdk.test.lib.crac.CracTest true      - pass
 * @run driver jdk.test.lib.crac.CracTest false     - fail
 * @run driver jdk.test.lib.crac.CracTest false true  fail
 * @run driver jdk.test.lib.crac.CracTest false false pass
 * @run driver jdk.test.lib.crac.CracTest true  true  pass
 * @run driver jdk.test.lib.crac.CracTest true  false fail
 */
/*
 * @test id=NO_PACA
 * @requires (os.family == "linux")
 * @requires (os.arch == "aarch64" & vm.cpu.features ~= ".*notpaca.*")
 * @library /test/lib
 * @build PointerAuthenticationTest
 * @run driver jdk.test.lib.crac.CracTest     - true  fail
 * @run driver jdk.test.lib.crac.CracTest     - false pass
 * @run driver jdk.test.lib.crac.CracTest true      - fail
 * @run driver jdk.test.lib.crac.CracTest false     - pass
 * @run driver jdk.test.lib.crac.CracTest false true  fail
 * @run driver jdk.test.lib.crac.CracTest false false pass
 */
public class PointerAuthenticationTest implements CracTest {

    @CracTestArg(0)
    String useBefore;

    @CracTestArg(1)
    String useAfter;

    @CracTestArg(2)
    String result;

    private CracBuilder setUsePAC(CracBuilder builder, String use) {
        return switch (use) {
            case "-" -> builder;
            case "true" -> builder.vmOption("-XX:+UsePAC");
            case "false" -> builder.vmOption("-XX:-UsePAC");
            case null, default -> throw new IllegalArgumentException(use);
        };
    }

    @Override
    public void test() throws Exception {
        setUsePAC(new CracBuilder(), useBefore).doCheckpoint();
        var rst = setUsePAC(new CracBuilder(), useAfter).doRestoreToAnalyze();
        switch (result) {
            case "pass" -> rst.shouldHaveExitValue(0);
            case "fail" -> rst.shouldHaveExitValue(1)
                    .stdoutShouldContain("Restore failed due to incompatible aarch64 CPU feature PACA");
            case "crash" -> rst.shouldHaveExitValue(132); // SIGILL
        }
    }

    @Override
    public void exec() throws Exception {
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
    }
}
