// Copyright 2019-2020 Azul Systems, Inc.  All Rights Reserved.
// DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
//
// This code is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License version 2 only, as published by
// the Free Software Foundation.
//
// This code is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License version 2 for more
// details (a copy is included in the LICENSE file that accompanied this code).
//
// You should have received a copy of the GNU General Public License version 2
// along with this work; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
//
// Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale,
// CA 94089 USA or visit www.azul.com if you need additional information or
// have any questions.

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.nio.channels.Selector;
import java.nio.channels.spi.SelectorProvider;

import static jdk.test.lib.Asserts.assertEquals;

/*
 * @test id=DEFAULT Selector/selectAfterWakeup
 * @summary check that the Selector's wakeup() makes the subsequent select() call to return immediately
 *          (see also jdk/test/java/nio/channels/Selector/WakeupSpeed.java);
 *          covers ZE-983
 * @library /test/lib
 * @build SelectAfterWakeupTest
 * @run driver jdk.test.lib.crac.CracTest true  false false
 * @run driver jdk.test.lib.crac.CracTest true  false true
 * @run driver jdk.test.lib.crac.CracTest true  true  false
 * @run driver jdk.test.lib.crac.CracTest true  true  true
 * @run driver jdk.test.lib.crac.CracTest false true  false
 * @run driver jdk.test.lib.crac.CracTest false true  true
 */
/*
 * @test id=ALT_UNIX
 * @requires (os.family != "windows")
 * @library /test/lib
 * @build SelectAfterWakeupTest
 * @run driver jdk.test.lib.crac.CracTest true  false false sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  false true  sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  true  false sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  true  true  sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest false true  false sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest false true  true  sun.nio.ch.PollSelectorProvider
 */
/*
 * @test id=ALT_WINDOWS
 * @requires (os.family == "windows")
 * @library /test/lib
 * @build SelectAfterWakeupTest
 * @run driver jdk.test.lib.crac.CracTest true  false false sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  false true  sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  true  false sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest true  true  true  sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest false true  false sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest false true  true  sun.nio.ch.WindowsSelectorProvider
 */
public class SelectAfterWakeupTest implements CracTest {
    @CracTestArg(0)
    boolean wakeupBeforeCheckpoint;

    @CracTestArg(1)
    boolean wakeupAfterRestore;

    @CracTestArg(2)
    boolean setSelectTimeout;

    @CracTestArg(value = 3, optional = true)
    String selectorImpl;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        if (selectorImpl != null) {
            builder.javaOption(SelectorProvider.class.getName(), selectorImpl);
        }
        builder.doCheckpoint();
    }

    @Override
    public void exec() throws Exception {
        if (selectorImpl != null) {
            assertEquals(selectorImpl, SelectorProvider.provider().getClass().getName());
        }

        Selector selector = Selector.open();

        // do this just in case
        selector.wakeup();
        selector.select();

        if (wakeupBeforeCheckpoint) {
            selector.wakeup();
        }

        jdk.crac.Core.checkpointRestore();

        if (wakeupAfterRestore) {
            selector.wakeup();
        }
        if (setSelectTimeout) { selector.select(3600_000); }
        else { selector.select(); }

        selector.close();
    }
}
