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
 * @test id=DEFAULT
 * @summary a trivial check that Selector.wakeup() after restore behaves as expected
 * @library /test/lib
 * @build SelectAndWarkeupAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest
 */
/*
 * @test id=ALT_LINUX
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build SelectAndWarkeupAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest sun.nio.ch.PollSelectorProvider
 */
/*
 * @test id=ALT_WINDOWS
 * @requires (os.family == "windows")
 * @library /test/lib
 * @build SelectAndWarkeupAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest sun.nio.ch.WindowsSelectorProvider
 */
public class SelectAndWarkeupAfterRestoreTest implements CracTest {
    @CracTestArg(optional = true)
    String selectorImpl;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        if (selectorImpl != null) {
            builder.javaOption(SelectorProvider.class.getName(), selectorImpl);
        }
        builder.startCheckpoint().waitForSuccess();
    }

    private static void selectAndWakeup(Selector selector) throws java.io.IOException {

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Thread.sleep(7000);
                    System.out.println(">> waking up");
                    selector.wakeup();
                } catch (InterruptedException ie) { throw new RuntimeException(ie); }
            }
        }).start();

        System.out.println(">> selecting");
        selector.select();
    }

    @Override
    public void exec() throws Exception {
        if (selectorImpl != null) {
            assertEquals(selectorImpl, SelectorProvider.provider().getClass().getName());
        }

        Selector selector = Selector.open();

        selectAndWakeup(selector); // just in case

        jdk.crac.Core.checkpointRestore();

        selectAndWakeup(selector);

        selector.close();
    }
}
