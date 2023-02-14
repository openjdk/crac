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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.crac.*;

public class RestoreEnvironmentTest {
    static public void main(String[] args) throws Exception {
        {
            String testVarName = "RESTORE_ENVIRONMENT_TEST_VAR";

            for (int i = 0; i < 3; ++i) {
                var testVar = java.lang.System.getenv(testVarName + i);
                System.out.println("(before checkpoint) " + testVarName + i + "=" + testVar);
            }

            jdk.crac.Core.checkpointRestore();

            System.out.print("(after restore) ");
            for (int i = 0; i < 3; ++i) {
                var testVar = java.lang.System.getenv(testVarName + i);
                System.out.print(testVarName + i + "=" + testVar + ";");
            }
        }
        System.out.println();
    }
}
