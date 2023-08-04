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

package jdk.crac.impl;

import java.util.concurrent.CountDownLatch;

/**
 * Keeps VM alive by at least one non-daemon thread.
 */
public class KeepAlive implements AutoCloseable {
    private final CountDownLatch start = new CountDownLatch(1);
    private final CountDownLatch finish = new CountDownLatch(1);
    private final Thread thread;

    public KeepAlive() {
        // When the thread running notifications is not a daemon thread
        // it is unnecessary to create the keep-alive thread.
        if (!Thread.currentThread().isDaemon()) {
            thread = null;
            return;
        }
        thread = new Thread(() -> {
            start.countDown();
            try {
                finish.await();
            } catch (InterruptedException e) {
                throw new RuntimeException(e);
            }
        }, "CRaC Keep-Alive");
        thread.setDaemon(false);
        thread.start();
        try {
            start.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new RuntimeException("Interrupted waiting for the keep-alive thread to start.", e);
        }
    }

    @Override
    public void close() {
        if (thread == null) {
            return; // noop
        }
        finish.countDown();
        try {
            thread.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new RuntimeException("Interrupted waiting for the keep-alive thread to complete", e);
        }
    }
}
