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
import java.io.IOException;
import java.nio.channels.spi.SelectorProvider;

import static jdk.test.lib.Asserts.assertEquals;

/*
 * @test id=DEFAULT
 * @summary check that the Selector selected before the checkpoint,
 *          will wake up by timeout after the restore
 * @library /test/lib
 * @build WakeupByTimeoutAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest
 */
/*
 * @test id=ALT_UNIX
 * @requires (os.family != "windows")
 * @library /test/lib
 * @build WakeupByTimeoutAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest sun.nio.ch.PollSelectorProvider
 */
/*
 * @test id=ALT_WINDOWS
 * @requires (os.family == "windows")
 * @library /test/lib
 * @build WakeupByTimeoutAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest sun.nio.ch.WindowsSelectorProvider
 */
public class WakeupByTimeoutAfterRestoreTest implements CracTest {

    private final static long TIMEOUT = 40_000; // 40 seconds

    static boolean awakened = false;

    @CracTestArg(optional = true)
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
        Runnable r = new Runnable() {
            @Override
            public void run() {
                try {
                    selector.select(TIMEOUT);
                    awakened = true;
                } catch (IOException e) { throw new RuntimeException(e); }
            }
        };
        Thread t = new Thread(r);
        t.start();
        Thread.sleep(1000);

        jdk.crac.Core.checkpointRestore();

        t.join();
        if (!awakened) { throw new RuntimeException("not awakened!"); }

        // check that the selector works as expected

        if (!selector.isOpen()) { throw new RuntimeException("the selector must be open"); }

        selector.wakeup();
        selector.select();

        selector.selectNow();
        selector.select(200);
        selector.close();
    }
}
