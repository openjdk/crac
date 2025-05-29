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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.crac.Core;
import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

/**
 * @test
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build NewPropertiesTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class NewPropertiesTest implements CracTest {
    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder();

        builder.javaOption("old", "old val");

        builder.doCheckpoint();

        builder.javaOption("old", "new val");
        builder.javaOption("new1", "foo=bar");
        builder.vmOption("-Dnew2=");
        builder.vmOption("-Dnew3");

        builder.doRestore();
    }

    @Override
    public void exec() throws Exception {
        Asserts.assertEquals(System.getProperty("old"), "old val");
        Asserts.assertNull(System.getProperty("new1"));
        Asserts.assertNull(System.getProperty("new2"));
        Asserts.assertNull(System.getProperty("new3"));

        Core.checkpointRestore();

        Asserts.assertEquals(System.getProperty("old"), "new val");
        Asserts.assertEquals(System.getProperty("new1"), "foo=bar");
        Asserts.assertEquals(System.getProperty("new2"), "");
        Asserts.assertEquals(System.getProperty("new3"), "");
    }
}
