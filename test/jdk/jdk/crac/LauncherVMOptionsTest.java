/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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
 */
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @build LauncherVMOptionsTest
 * @run driver jdk.test.lib.crac.CracTest NONE
 * @run driver jdk.test.lib.crac.CracTest REGULAR
 * @run driver jdk.test.lib.crac.CracTest VM_OPTION
 * @run driver jdk.test.lib.crac.CracTest FILE
 * @requires (os.family == "linux")
 */
public class LauncherVMOptionsTest implements CracTest {
    private static final String CHECKPOINT_OPT = "-XX:CRaCCheckpointTo=cr";
    private static final String CR_VM_OPTIONS = "cr_vm_options";

    enum Variant {
        NONE,
        REGULAR,
        VM_OPTION,
        FILE,
    }

    @CracTestArg
    private Variant variant;

    @Override
    public void test() throws Exception {
        // Let's prevent inheriting any GLIBC_TUNABLES value...
        assertNull(System.getenv("GLIBC_TUNABLES"));
        CracBuilder builder = new CracBuilder();
        switch (variant) {
            case REGULAR -> builder.vmOption(CHECKPOINT_OPT);
            case VM_OPTION -> builder.env("JDK_JAVA_OPTIONS", CHECKPOINT_OPT);
            case FILE -> {
                Files.writeString(Path.of(CR_VM_OPTIONS), CHECKPOINT_OPT + "\n");
                builder.vmOption("@" + CR_VM_OPTIONS);
            }
        }
        builder.doPlain();
    }

    @Override
    public void exec() throws Exception {
        // When the launcher parses args/VM options and detects that we're running in a configuration
        // allowing checkpoint, it will set GLIBC_TUNABLES=glibc.pthread.rseq=0
        String tunables = System.getenv("GLIBC_TUNABLES");
        if (variant != Variant.NONE) {
            assertNotNull(tunables);
        } else {
            assertNull(tunables);
        }
    }
}
