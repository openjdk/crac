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

import jdk.crac.Resource;

public interface JDKResource extends Resource {
    /**
     * JDK Resource priorities.
     * Most resources should use priority NORMAL.
     * Other priorities define sequence of checkpoint notification
     * for dependent resources.
     * If priority A is specified early in the enumeration than priority B,
     * a JDK resource with priority A will be notified about checkpoint
     * later than JDK resource with priority B. When restoring, the order
     * is reversed: JDK resource with priority A will be notified about
     * restore early than JDK resource with priority B.
     * JDK resources with the same priority will be notified about checkpoint
     * in the reverse order of registration.
     * JDK resources with the same priority will be notified about restore
     * in the direct order of registration.
     */
    enum Priority {
        /**
         * Priority of the
         * sun.nio.ch.EPollSelectorImpl resource
         */
        EPOLLSELECTOR,
        /**
         * Most resources should use this option.
         */
        NORMAL
    };

    Priority getPriority();
}
