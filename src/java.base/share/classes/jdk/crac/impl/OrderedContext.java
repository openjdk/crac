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

import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

public class OrderedContext<R extends Resource> extends AbstractContextImpl<R> {
    private final String name;
    private boolean checkpointing = false;
    protected long order = 0;
    protected final WeakHashMap<R, Long> resources = new WeakHashMap<>();

    public OrderedContext() {
        this(null);
    }

    public OrderedContext(String name) {
        this.name = name;
    }

    @Override
    public String toString() {
        return name != null ? name : super.toString();
    }

    @Override
    public synchronized void register(R r) {
        resources.put(r, order++);
        // It is possible that something registers to us during restore but before
        // this context's afterRestore was called.
        if (checkpointing && !Core.isRestoring()) {
            setModified(r, null);
        }
    }

    @Override
    protected void runBeforeCheckpoint() {
        List<R> resources;
        synchronized (this) {
            checkpointing = true;
            resources = this.resources.entrySet().stream()
                    .sorted(Collections.reverseOrder(Map.Entry.comparingByValue()))
                    .map(Map.Entry::getKey)
                    .toList();
        }
        for (R r : resources) {
            invokeBeforeCheckpoint(r);
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) {
        synchronized (this) {
            checkpointing = false;
        }
        super.afterRestore(context);
    }
}
