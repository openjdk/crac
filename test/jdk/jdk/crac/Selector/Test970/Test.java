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

class ChannelResource implements jdk.crac.Resource {

    public enum SelectionType {SELECT, SELECT_TIMEOUT, SELECT_NOW};

    private SocketChannel channel;
    private SelectionKey  key;
    private Selector      selector;

    private final SelectionType selType;

    public ChannelResource(SelectionType selType) {
        this.selType = selType;
        jdk.crac.Core.getGlobalContext().register(this);
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
    public void beforeCheckpoint() throws IOException {

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
                    } catch (InterruptedException ie) { throw new RuntimeException(ie); }
                }
            }).start();

            selector.select();
        }
    }

    @Override
    public void afterRestore() {}
}


public class Test {

    private static void Test(ChannelResource.SelectionType selType, boolean openSelectorAtFirst) throws Exception {

        if (openSelectorAtFirst) {

            Selector selector = Selector.open();
            ChannelResource ch = new ChannelResource(selType);
            ch.open();
            ch.register(selector);

            jdk.crac.Core.checkpointRestore();

            selector.close();

        } else { // try in other order (see ZE-970)

            ChannelResource ch = new ChannelResource(selType);
            ch.open();
            Selector selector = Selector.open();
            ch.register(selector);

            jdk.crac.Core.checkpointRestore();

            selector.close();
        }
    }


    public static void main(String args[]) throws Exception {

        if (args.length < 1) { throw new RuntimeException("test number is missing"); }

        switch (args[0]) { // 1, 2: ZE-970
            case "1":
                Test(ChannelResource.SelectionType.SELECT_NOW, true);
                break;
            case "2":
                Test(ChannelResource.SelectionType.SELECT_NOW, false);
                break;
            case "3":
                Test(ChannelResource.SelectionType.SELECT, true);
                break;
            case "4":
                Test(ChannelResource.SelectionType.SELECT, false);
                break;
            case "5":
                Test(ChannelResource.SelectionType.SELECT_TIMEOUT, true);
                break;
            case "6":
                Test(ChannelResource.SelectionType.SELECT_TIMEOUT, false);
                break;
            default:
                throw new RuntimeException("invalid test number");
        }

    }
}
