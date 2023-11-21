/*
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
 */

import java.io.File;

import jdk.crac.Core;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

 /*
 * @test
 * @library /test/lib
 * @build JvmtiEventTest
 * @run driver jdk.test.lib.crac.CracTest
 */

public class JvmtiEventTest implements CracTest {

    private static final String JAVA_LIBRARY_PATH = System.getProperty("java.library.path") + File.separator;
    private static final String AGENT_CALLBACK_BEFORE_CHECKPOINT = "callbackBeforeCheckpoint";
    private static final String AGENT_CALLBACK_AFTER_RESTORE = "callbackAfterRestore";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        builder.vmOption("-agentpath:" + JAVA_LIBRARY_PATH + System.mapLibraryName("CracJvmtiAgent"));

        CracProcess process = builder.captureOutput(true).startCheckpoint();
        process.waitForSuccess();
        process.outputAnalyzer()
                .shouldContain(AGENT_CALLBACK_BEFORE_CHECKPOINT)
                .shouldContain(AGENT_CALLBACK_AFTER_RESTORE)
                .shouldContain(RESTORED_MESSAGE);
    }

    @Override
    public void exec() throws Exception {
        System.out.println("Started");
        Core.checkpointRestore();
        System.out.println(RESTORED_MESSAGE);
    }
}
