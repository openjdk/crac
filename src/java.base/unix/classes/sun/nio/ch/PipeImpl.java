/*
 * Copyright (c) 2000, 2018, Oracle and/or its affiliates. All rights reserved.
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

package sun.nio.ch;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenResourceException;
import jdk.internal.crac.OpenResourcePolicies;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKFdResource;
import jdk.internal.crac.LoggerContainer;

import java.io.*;
import java.nio.channels.*;
import java.nio.channels.spi.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

class PipeImpl
    extends Pipe
{

    // Source and sink channels
    private final SourceChannelImpl source;
    private final SinkChannelImpl sink;
    private final JDKFdResource resource = new JDKFdResource() {
        @SuppressWarnings("fallthrough")
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            OpenResourcePolicies.Policy policy = OpenResourcePolicies.find(false, OpenResourcePolicies.PIPE, null);
            String action = policy != null ? policy.action.toLowerCase() : "error";
            switch (action) {
                case "error":
                    // We will report the error only once
                    AtomicBoolean reported = new AtomicBoolean();
                    Supplier<Exception> supplier = () -> reported.getAndSet(true) ? null :
                            new CheckpointOpenResourceException(toString(), getStackTraceHolder());
                    Core.getClaimedFDs().claimFd(source.getFD(), this, supplier, source.getFD());
                    Core.getClaimedFDs().claimFd(sink.getFD(), this, supplier, sink.getFD());
                    break;
                case "close":
                    source.close();
                    sink.close();
                    // intentional fallthrough
                case "ignore":
                    if (Boolean.parseBoolean(policy.params.getOrDefault("warn", "false"))) {
                        LoggerContainer.warn("CRaC: {0} was not closed by the application!", this);
                    }
                    Core.getClaimedFDs().claimFd(source.getFD(), this, NO_EXCEPTION, source.getFD());
                    Core.getClaimedFDs().claimFd(sink.getFD(), this, NO_EXCEPTION, sink.getFD());
                default:
                    throw new IllegalStateException("Unknown policy action " + action + " for " + PipeImpl.this, null);
            }
        }
    };

    @Override
    public String toString() {
        return "Pipe " + source.getFDVal() + " -> " + sink.getFDVal();
    }

    PipeImpl(SelectorProvider sp) throws IOException {
        long pipeFds = IOUtil.makePipe(true);
        int readFd = (int) (pipeFds >>> 32);
        int writeFd = (int) pipeFds;
        FileDescriptor sourcefd = new FileDescriptor();
        IOUtil.setfdVal(sourcefd, readFd);
        source = new SourceChannelImpl(sp, sourcefd);
        FileDescriptor sinkfd = new FileDescriptor();
        IOUtil.setfdVal(sinkfd, writeFd);
        sink = new SinkChannelImpl(sp, sinkfd);
    }

    public SourceChannel source() {
        return source;
    }

    public SinkChannel sink() {
        return sink;
    }
}
