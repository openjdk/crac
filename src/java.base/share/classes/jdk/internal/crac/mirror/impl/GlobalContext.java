/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
package jdk.internal.crac.mirror.impl;

import jdk.internal.crac.JDKResource;
import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;

public class GlobalContext {
    private static final String GLOBAL_CONTEXT_IMPL_PROP = "jdk.crac.globalContext.impl";

    // Strong reference to the resource
    private static Score scoreSingleton;

    public static class Score implements JDKResource {
        private final String name;
        private final OrderedContext<?> ctx;

        private Score(String name, OrderedContext<?> ctx) {
            this.name = name;
            this.ctx = ctx;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            jdk.internal.crac.Score.setScore(name + ".size", ctx.size());
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }
    }

    private GlobalContext() {}

    public static Context<Resource> createGlobalContextImpl(String name) {
        String implName = System.getProperty(GLOBAL_CONTEXT_IMPL_PROP, "");
        OrderedContext<Resource> ctx = switch (implName) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> new OrderedContext<>(); // cannot report as System.out/err are not initialized yet
        };
        // The 'internal' context from jdk.internal.crac.mirror.Core should host only the Core.Priority contexts
        // and the context created by jdk.crac.Core (the 'user' global context). We won't record score for
        // the internal context as if registered here, beforeCheckpoint would be called after Core.Priority.SCORE
        // and the score would not be recorded.
        if (name != null) {
            Score score = new Score(name, ctx);
            synchronized (GlobalContext.class) {
                // In JDK this should be called only once with a non-null name. If the implementation changes
                // let's turn scoreSingleton into a map.
                assert scoreSingleton == null;
                scoreSingleton = score;
            }
            ctx.register(score);
        }
        return ctx;
    }
}
