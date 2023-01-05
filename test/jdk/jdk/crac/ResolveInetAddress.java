/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.file.Files;
import java.nio.file.Path;

public class ResolveInetAddress {
    public static void main(String[] args) {
        if (args.length < 2) {
            System.err.println("Args: <ip address> <check file path>");
            return;
        }
        printAddress(args[0]);
        while (!Files.exists(Path.of(args[1]))) {
            try {
                //noinspection BusyWait
                Thread.sleep(100);
            } catch (InterruptedException e) {
                System.err.println("Interrupted!");
                return;
            }
        }
        printAddress(args[0]);
    }

    private static void printAddress(String hostname) {
        try {
            InetAddress address = InetAddress.getByName(hostname);
            // we will assume IPv4 address
            byte[] bytes = address.getAddress();
            System.out.print(bytes[0] & 0xFF);
            for (int i = 1; i < bytes.length; ++i) {
                System.out.print('.');
                System.out.print(bytes[i] & 0xFF);
            }
            System.out.println();
        } catch (UnknownHostException e) {
            System.out.println();
        }
    }
}
