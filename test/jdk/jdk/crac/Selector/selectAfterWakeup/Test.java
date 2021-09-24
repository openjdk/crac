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


public class Test {

    private static void test(boolean wakeupBeforeCheckpoint,
                             boolean wakeupAfterRestore,
                             boolean setSelectTimeout) throws Exception {

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

    public static void main(String args[]) throws Exception {

        if (args.length < 1) { throw new RuntimeException("test number is missing"); }

        switch (args[0]) {
            case "1":
                test(true, false, false); // ZE-983
                break;
            case "2":
                test(true, false, true);
                break;
            case "3":
                test(true, true, false); // ZE-983
                break;
            case "4":
                test(true, true, true);
                break;
            case "5":
                test(false, true, false);
                break;
            case "6":
                test(false, true, true);
                break;

            default:
                throw new RuntimeException("invalid test number");
        }
    }
}
