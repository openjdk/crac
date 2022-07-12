/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.*;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

/**
 * @test
 * @library /test/lib
 * @run main LazyProps
 */
public class LazyProps {
    static public void main(String[] args) throws Exception {
        if (args.length == 0) {
            OutputAnalyzer output = ProcessTools.executeTestJvm(
                    "-XX:CREngine=simengine", "-XX:CRaCCheckpointTo=./cr",
                    "LazyProps",
                    "-runTest");
            output.shouldHaveExitValue(0);
            output.shouldContain("jdk.crac beforeCheckpoint");
        } else {
            Resource resource = new Resource() {
                @Override
                public void beforeCheckpoint(Context<? extends Resource> context) throws Exception { }
                @Override
                public void afterRestore(Context<? extends Resource> context) throws Exception { }
            };
            Core.getGlobalContext().register(resource);

            System.setProperty("jdk.crac.debug", "true");
            Core.checkpointRestore();
        }
    }
}
