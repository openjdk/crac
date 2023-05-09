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

package jdk.internal.crac;

import jdk.crac.*;
import jdk.crac.impl.AbstractContextImpl;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import sun.security.action.GetBooleanAction;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;
import java.util.*;
import java.util.function.Supplier;
import java.util.stream.Collectors;

public class JDKContext extends AbstractContextImpl<JDKResource, Void> {
    private static class ContextComparator implements Comparator<Map.Entry<JDKResource, Void>> {
        @Override
        public int compare(Map.Entry<JDKResource, Void> o1, Map.Entry<JDKResource, Void> o2) {
            return o1.getKey().getPriority().compareTo(o2.getKey().getPriority());
        }
    }

    private static final String COLLECT_FD_STACKTRACES_PROPERTY = "jdk.crac.collect-fd-stacktraces";
    private static final String COLLECT_FD_STACKTRACES_HINT =
        "Set System Property " + COLLECT_FD_STACKTRACES_PROPERTY + "=true to get stack traces of JDK Resources";

    // JDKContext by itself is initialized too early when system properties are not set yet
    public static class Properties {
        public static final boolean COLLECT_FD_STACKTRACES =
                GetBooleanAction.privilegedGetProperty(JDKContext.COLLECT_FD_STACKTRACES_PROPERTY);

        public static final String[] classpathEntries =
                GetPropertyAction.privilegedGetProperty("java.class.path")
                .split(File.pathSeparator);
    }

    private WeakHashMap<FileDescriptor, Supplier<Exception>> claimedFds;

    public boolean matchClasspath(String path) {
        for (String cp : Properties.classpathEntries) {
            if (cp.equals(path)) {
                return true;
            }
        }
        return false;
    }

    JDKContext() {
        super(new ContextComparator());
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        claimedFds = new WeakHashMap<>();
        try {
            super.beforeCheckpoint(context);
        } catch (CheckpointException ce) {
            if (!Properties.COLLECT_FD_STACKTRACES) {
                LoggerContainer.info(COLLECT_FD_STACKTRACES_HINT);
            }
            throw ce;
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        super.afterRestore(context);
        claimedFds = null;
    }

    @Override
    public void register(JDKResource resource) {
        register(resource, null);
    }

    public Map<Integer, Supplier<Exception>> getClaimedFds() {
        JavaIOFileDescriptorAccess fileDescriptorAccess = SharedSecrets.getJavaIOFileDescriptorAccess();
        return claimedFds.entrySet().stream()
                .collect(Collectors.toMap(entry -> fileDescriptorAccess.get(entry.getKey()), Map.Entry::getValue));
    }

    public void claimFd(FileDescriptor fd, Supplier<Exception> supplier, Class<?>... supresses) {
        Objects.requireNonNull(supplier);

        if (fd == null || !fd.valid()) {
            return;
        }

        Object old = claimedFds.putIfAbsent(fd, supplier);
        if (old == null) {
            return;
        }

        for (Class<?> c : supresses) {
            // if C is subclass of Old
            if (old.getClass().isAssignableFrom(c)) {
                claimedFds.put(fd, supplier);
                break;
            }
        }
    }
}
