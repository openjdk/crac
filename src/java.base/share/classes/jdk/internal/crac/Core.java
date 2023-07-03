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

import jdk.crac.Context;
import jdk.crac.impl.BlockingOrderedContext;

public class Core {
    private static ClaimedFDs claimedFDs;

    /**
     * Called by JDK FD resources
     * @return
     */
    public static ClaimedFDs getClaimedFDs() {
        return claimedFDs;
    }

    /**
     * Called by jdk.crac.Core to publish current ClaimedFDs
     */
    public static void setClaimedFDs(ClaimedFDs fds) {
        claimedFDs = fds;
    }

    /**
     * Priorities are defined in the order of registration to the global context.
     * Checkpoint notification will be processed in the order from the bottom to the top of the list.
     * Restore will be done in reverse order: from the top to the bottom.
     *
     * Resources of the same priority will be handled according the context supplied to the priority.
     *
     * Most resources should use priority NORMAL (the lowest priority).
     */
    public enum Priority {
        FILE_DESCRIPTORS(new BlockingOrderedContext<>()),
        CLEANERS(new BlockingOrderedContext<>()),
        REFERENCE_HANDLER(new BlockingOrderedContext<>()),
        SEEDER_HOLDER(new BlockingOrderedContext<>()),
        SECURE_RANDOM(new BlockingOrderedContext<>()),
        NATIVE_PRNG(new BlockingOrderedContext<>()),
        EPOLLSELECTOR(new BlockingOrderedContext<>()),
        NORMAL(new BlockingOrderedContext<>());

        private final Context<JDKResource> context;
        Priority(Context<JDKResource> context) {
            jdk.crac.Core.getGlobalContext().register(context);
            this.context = context;
        }

        public Context<JDKResource> getContext() {
            return context;
        }
    }
}
