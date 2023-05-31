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

import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import sun.security.action.GetBooleanAction;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;
import java.util.Map;
import java.util.Objects;
import java.util.WeakHashMap;
import java.util.function.Supplier;
import java.util.stream.Collectors;

public class ClaimedFDs {
    public static final String COLLECT_FD_STACKTRACES_PROPERTY = "jdk.crac.collect-fd-stacktraces";
    public static final String COLLECT_FD_STACKTRACES_HINT = "Use -D" + COLLECT_FD_STACKTRACES_PROPERTY + "=true to find the source.";

    // JDKContext by itself is initialized too early when system properties are not set yet
    public static class Properties {
        public static final boolean COLLECT_FD_STACKTRACES =
                GetBooleanAction.privilegedGetProperty(ClaimedFDs.COLLECT_FD_STACKTRACES_PROPERTY);

        public static final String[] classpathEntries =
                GetPropertyAction.privilegedGetProperty("java.class.path")
                .split(File.pathSeparator);
    }

    private final WeakHashMap<FileDescriptor, Tuple<Object, Supplier<Exception>>> fds = new WeakHashMap<>();

    public boolean matchClasspath(String path) {
        for (String cp : Properties.classpathEntries) {
            if (cp.equals(path)) {
                return true;
            }
        }
        return false;
    }

    public Map<Integer, Supplier<Exception>> getClaimedFds() {
        JavaIOFileDescriptorAccess fileDescriptorAccess = SharedSecrets.getJavaIOFileDescriptorAccess();
        return fds.entrySet().stream()
            .filter((var e) -> e.getKey().valid())
            .collect(Collectors.toMap(entry -> fileDescriptorAccess.get(entry.getKey()), entry -> entry.getValue().second()));
    }

    private static class Tuple<T1, T2> {
        private final T1 o1;
        private final T2 o2;

        public Tuple(T1 o1, T2 o2) {
            this.o1 = o1;
            this.o2 = o2;
        }
        public T1 first() { return o1; }
        public T2 second() { return o2; }
    }

    public void claimFd(FileDescriptor fd, Object claimer, Object suppressedClaimer, Supplier<Exception> supplier) {
        Objects.requireNonNull(supplier);

        if (fd == null) {
            return;
        }

        Tuple<Object, Supplier<Exception>> record = fds.get(fd);
        if (record == null || suppressedClaimer == record.first()) {
            fds.put(fd, new Tuple<>(claimer, supplier));
        }
    }
}
