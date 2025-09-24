/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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
package jdk.test.lib.crac;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

public class AsyncStreamReader {
    private final LinkedBlockingQueue<String> lines = new LinkedBlockingQueue<>();
    private volatile boolean isRunning = true;

    public AsyncStreamReader(InputStream stream) {
        Thread t = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream))) {
                for (String line; (line = reader.readLine()) != null;) {
                    lines.put(line);
                }
            } catch (IOException | InterruptedException e) {
                e.printStackTrace();
            } finally {
                isRunning = false;
            }
        });
        t.setDaemon(true);
        t.start();
    }

    public String readLine(long timeoutMillis) throws TimeoutException, InterruptedException {
        String line = lines.poll(timeoutMillis, TimeUnit.MILLISECONDS);
        if (line == null) {
            throw new TimeoutException("Timeout " + timeoutMillis + " msecs while waiting for new line");
        }
        return line;
    }

    public boolean isRunning() {
        return isRunning;
    }
}
