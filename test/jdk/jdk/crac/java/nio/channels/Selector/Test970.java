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

import java.nio.channels.*;
import java.io.IOException;
import java.nio.channels.spi.SelectorProvider;

import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import static jdk.test.lib.Asserts.assertEquals;

/*
 * @test id=DEFAULT
 * @summary a regression test for ZE-970 ("a channel deregistration
 *          is locked depending on mutual order of selector and channel creation")
 * @library /test/lib
 * @build Test970
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW true
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW false
 * @run driver jdk.test.lib.crac.CracTest SELECT true
 * @run driver jdk.test.lib.crac.CracTest SELECT false
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT true
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT false
 */
/*
 * @test id=ALT_UNIX
 * @requires (os.family != "windows")
 * @library /test/lib
 * @build Test970
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW     true  sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW     false sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT         true  sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT         false sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT true  sun.nio.ch.PollSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT false sun.nio.ch.PollSelectorProvider
 */
/*
 * @test id=ALT_WINDOWS
 * @requires (os.family == "windows")
 * @library /test/lib
 * @build Test970
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW     true  sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_NOW     false sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT         true  sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT         false sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT true  sun.nio.ch.WindowsSelectorProvider
 * @run driver jdk.test.lib.crac.CracTest SELECT_TIMEOUT false sun.nio.ch.WindowsSelectorProvider
 */
public class Test970 implements CracTest {
    @CracTestArg(0)
    ChannelResource.SelectionType selType;

    @CracTestArg(1)
    boolean openSelectorAtFirst;

    @CracTestArg(value = 2, optional = true)
    String selectorImpl;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        if (selectorImpl != null) {
            builder.javaOption(SelectorProvider.class.getName(), selectorImpl);
        }
        builder.startCheckpoint().waitForSuccess();
    }

    @Override
    public void exec() throws Exception {
        if (selectorImpl != null) {
            assertEquals(selectorImpl, SelectorProvider.provider().getClass().getName());
        }

        if (openSelectorAtFirst) {

            Selector selector = Selector.open();
            ChannelResource ch = new ChannelResource(selType);
            ch.open();
            ch.register(selector);

            Core.checkpointRestore();

            selector.close();

        } else { // try in other order (see ZE-970)

            ChannelResource ch = new ChannelResource(selType);
            ch.open();
            Selector selector = Selector.open();
            ch.register(selector);

            Core.checkpointRestore();

            selector.close();
        }
    }

    static class ChannelResource implements Resource {

        public enum SelectionType {
            SELECT,
            SELECT_TIMEOUT,
            SELECT_NOW
        };

        private SocketChannel channel;
        private SelectionKey key;
        private Selector selector;

        private final SelectionType selType;

        public ChannelResource(SelectionType selType) {
            this.selType = selType;
            Core.getGlobalContext().register(this);
        }

        public void open() throws IOException {
            channel = SocketChannel.open();
            channel.configureBlocking(false);
        }

        public void register(Selector selector) throws IOException {
            key = channel.register(selector, SelectionKey.OP_READ);
            this.selector = selector;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws IOException {

            channel.socket().close();

            // causes the channel deregistration
            if (selType == SelectionType.SELECT_NOW) {
                selector.selectNow();
            } else if (selType == SelectionType.SELECT_TIMEOUT) {
                selector.select(500);
            } else {
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            Thread.sleep(1000);
                            selector.wakeup();
                        } catch (InterruptedException ie) {
                            throw new RuntimeException(ie);
                        }
                    }
                }).start();

                selector.select();
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }
    }
}
