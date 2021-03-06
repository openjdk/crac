/*
 * Copyright (c) 2019, 2022, Oracle and/or its affiliates. All rights reserved.
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
import java.io.File;
import java.util.List;
import java.util.Scanner;
import jdk.test.whitebox.WhiteBox;

public class LoadClasses {

    public static void main (String[] args) throws Throwable {
        String classList = args[0];
        Scanner sc = new Scanner(new File(classList));
        WhiteBox wb = WhiteBox.getWhiteBox();
        int count = 0;
        while (sc.hasNextLine()) {
            String cn = sc.nextLine().replace('/', '.');
            try {
                Class<?> cls = Class.forName(cn, false, LoadClasses.class.getClassLoader());
                wb.linkClass(cls);
                count++;
            } catch (Throwable ex) {
                System.out.println("Loading failed: " + cn);
                System.out.println(ex.toString());
            }
        }
        System.out.println("Loaded classes = " + count);
    }
}
