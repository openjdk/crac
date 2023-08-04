/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package jdk.internal.crac;

/**
 * Some classes that could use logging are initialized early during the boot
 * and keeping the logger in static final field there could cause problems
 * (e.g. recursion when service-loading logger implementation).
 * Therefore, we isolate the logger into a subclass and initialize lazily.
 */
public class LoggerContainer {
    public static final System.Logger logger = System.getLogger("jdk.crac");

    public static void info(String msg) {
        logger.log(System.Logger.Level.INFO, msg);
    }

    public static void debug(String fmt, Object... params) {
        logger.log(System.Logger.Level.DEBUG, fmt, params);
    }

    private LoggerContainer() {}

    public static void warn(String fmt, Object... params) {
        logger.log(System.Logger.Level.WARNING, fmt, params);
    }

    public static void error(String msg) {
        logger.log(System.Logger.Level.ERROR, msg);
    }

    public static void error(String fmt, Object... params) {
        logger.log(System.Logger.Level.ERROR, fmt, params);
    }

    public static void error(Throwable t, String msg) {
        logger.log(System.Logger.Level.ERROR, msg, t);
    }
}
