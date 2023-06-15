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

import java.io.FileDescriptor;
import java.util.List;
import java.util.Objects;
import java.util.WeakHashMap;
import java.util.function.Supplier;
import java.util.stream.Collectors;
import java.util.stream.Stream;


public class ClaimedFDs {
    private final static JavaIOFileDescriptorAccess fileDescriptorAccess = SharedSecrets.getJavaIOFileDescriptorAccess();

    private WeakHashMap<FileDescriptor, Descriptor> fds = new WeakHashMap<>();

    public static class Descriptor {
        private int fd;
        private final Object claimer;
        private final Supplier<Exception> exceptionSupplier;

        public Descriptor(Object claimer, Supplier<Exception> exceptionSupplier) {
            this.fd = -1;
            this.claimer = claimer;
            this.exceptionSupplier = exceptionSupplier;
        }

        void setFd(int fd) {
            assert this.fd == -1;
            this.fd = fd;
        }

        public int getFd() {
            assert this.fd != -1;
            return fd;
        }

        public Object getClaimer() {
            return claimer;
        }

        public Supplier<Exception> getExceptionSupplier() {
            return exceptionSupplier;
        }

        @Override
        public String toString() {
            return "{fd=" + fd + ", claimer=" + claimer + '}';
        }
    }

    public List<Descriptor> getClaimedFds() {
        List<Descriptor> list = fds.entrySet().stream()
            .filter((var e) -> e.getKey().valid())
            .map(entry -> {
                    Descriptor d = entry.getValue();
                    d.setFd(fileDescriptorAccess.get(entry.getKey()));
                    return d;
                })
            .collect(Collectors.toList());
        // destroy fds since we've modified Descriptors
        fds = null;
        return list;
    }

    public void claimFd(FileDescriptor fd, Object claimer, Supplier<Exception> supplier, Object... suppressedClaimers) {
        if (fd == null) {
            return;
        }

        Descriptor descriptor = fds.get(fd);
        LoggerContainer.debug("ClaimFD: fd {0} claimer {1} existing {2}",
            fd, claimer, descriptor != null ? descriptor.claimer : "NONE");
        if (descriptor == null ||
                Stream.of(suppressedClaimers).anyMatch((supressed) -> supressed == descriptor.getClaimer())) {
            fds.put(fd, new Descriptor(claimer, supplier));
        }
    }
}
