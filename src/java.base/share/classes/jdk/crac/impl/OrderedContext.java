/*
 * Copyright (c) 2019, 2021, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

import jdk.crac.*;

import java.util.*;

/**
 * Context performing Checkpoint notification in reverse order of registration.
 * Concurrent registration along notification is silently ignored.
 * @param <R>
 */
public class OrderedContext<R extends Resource> extends AbstractContext<R> {
    private final WeakHashMap<R, Long> resources = new WeakHashMap<>();
    private long order = 0;
    private List<R> restoreSnapshot = null;

    protected List<R> checkpointSnapshot() {
        List<R> snapshot;
        synchronized (this) {
            snapshot = this.resources.entrySet().stream()
                .sorted(Collections.reverseOrder(Map.Entry.comparingByValue()))
                .map(Map.Entry::getKey)
                .toList();
        }
        restoreSnapshot = new ArrayList<>(snapshot);
        Collections.reverse(restoreSnapshot);
        return snapshot;
    }

    // We won't synchronize access to restoreSnapshot because methods
    // beforeCheckpoint and afterRestore should be invoked only
    // by the single thread performing the C/R and other threads should
    // not touch that.
    protected List<R> restoreSnapshot() {
        List<R> snapshot = restoreSnapshot;
        restoreSnapshot = null;
        return snapshot;
    }

    @Override
    public synchronized void register(R resource) {
        resources.put(resource, order++);
    }
}
