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


import java.nio.channels.Selector;
import java.io.IOException;

public class Test {

    static boolean awakened, closed;

    private static void test(boolean setTimeout, boolean skipCR) throws Exception {

        Selector selector = Selector.open();

        Thread tSelect = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        awakened = false;
                        if (setTimeout) { selector.select(3600_000); }
                        else { selector.select(); }
                        awakened = true;
                    } catch (IOException e) { throw new RuntimeException(e); }
                }
            });
        tSelect.start();

        Thread.sleep(3000);

        if (!skipCR) { jdk.crac.Core.checkpointRestore(); }

        // close() must wakeup the selector
        Thread tClose = new Thread(new Runnable() {

                @Override
                public void run() {
                    try {
                        closed = false;
                        selector.close();
                        closed = true;
                    } catch (IOException e) { throw new RuntimeException(e); }
                }
            });
        tClose.start();
        tClose.join();
        tSelect.join();

        if (!awakened) {
            selector.wakeup();
            throw new RuntimeException("selector did not wake up");
        }

        if (!closed) {
            selector.close();
            throw new RuntimeException("selector did not close");
        }
    }

    public static void main(String[] args) throws Exception {

       if (args.length < 1) { throw new RuntimeException("test number is missing"); }

        switch (args[0]) {
            case "1":
                test(true, false);
                break;
            case "2":
                test(false, false);
                break;
            // 3, 4: skip C/R
            case "3":
                test(true, true);
                break;
            case "4":
                test(false, true);
                break;
            default:
                throw new RuntimeException("invalid test number");
        }
    }
}
