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

import jdk.crac.impl.CheckpointOpenSocketException;

import java.io.IOException;
import java.net.*;

public abstract class JDKSocketResource extends JDKSocketResourceBase {

    private SocketAddress local;
    private SocketAddress remote;

    public JDKSocketResource(Object owner) {
        super(owner);
    }

    protected abstract SocketAddress localAddress() throws IOException;
    protected abstract SocketAddress remoteAddress() throws IOException;

    @Override
    protected OpenResourcePolicies.Policy findPolicy(boolean isRestore) throws CheckpointOpenSocketException {
        if (!isRestore) {
            try {
                local = localAddress();
            } catch (IOException e) {
                throw new CheckpointOpenSocketException("Cannot find local address for " + owner, e);
            }
            try {
                remote = remoteAddress();
            } catch (IOException e) {
                throw new CheckpointOpenSocketException("Cannot find remote address for " + owner, e);
            }
        }
        var localMatcher = getMatcher(local, "localAddress", "localPort", "localPath");
        var remoteMatcher = getMatcher(remote, "remoteAddress", "remotePort", "remotePath");
        return OpenResourcePolicies.find(isRestore, OpenResourcePolicies.SOCKET,
                params -> localMatcher.test(params) && remoteMatcher.test(params));
    }

    @Override
    protected void reset() {
        // Allow garbage collection
        local = null;
        remote = null;
    }
}
