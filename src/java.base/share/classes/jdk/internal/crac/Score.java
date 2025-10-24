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
package jdk.internal.crac;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;
import jdk.internal.crac.mirror.impl.OrderedContext;

import java.util.HashMap;
import java.util.Map;

/**
 * CRaC Engine can support storing additional metadata about the image.
 * This can help the infrastructure to further refine the set of feasible images
 * (constrained by CPU architecture and features) and select the image that is expected to perform best.
 */
public class Score {
    private static final Map<String, Double> score = new HashMap<>();

    // There shouldn't be anyone else to register at Core.Priority.SCORE.
    // We need to class-load this class every time, to store common metrics.
    private static final Context<JDKResource> resource = new Context<>() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            if (isSupported()) {
                setJdkResourceScore();
                record();
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }

        @Override
        public void register(JDKResource resource) {
            throw new UnsupportedOperationException("Score is a singleton resource");
        }
    };

    private static void setJdkResourceScore() {
        int resources = 0;
        for (var p : Core.Priority.values()) {
            if (p.getContext() instanceof OrderedContext<?> octx) {
                resources += octx.size();
            }
        }
        setScore("jdk.crac.internalResources", resources);
    }

    private Score() {}

    public static native boolean isSupported();

    private static native boolean record(String[] metrics, double[] values);

    private static synchronized void record() {
        String[] metrics = new String[score.size()];
        double[] values = new double[score.size()];
        int i = 0;
        for (var e : score.entrySet()) {
            metrics[i] = e.getKey();
            values[i] = e.getValue();
            ++i;
        }
        if (!record(metrics, values)) {
            throw new RuntimeException("Cannot record image score");
        }
    }

    /**
     * Record value to be stored in the image metadata on checkpoint. Repeated invocations
     * with the same {@code metric} overwrite previous value. This method can be safely
     * called even if the C/R engine does not support recording metadata.
     * On checkpoint the metrics are not reset; if that is desired invoke {@link #resetAll()}
     * manually.
     *
     * @param metric Name of the metric.
     * @param value Numeric value of the metric.
     */
    public static synchronized void setScore(String metric, double value) {
        score.put(metric, value);
    }

    /**
     * Drop all currently stored metrics. This method can be safely called
     * even if the C/R engine does not support recording metadata.
     */
    public static synchronized void resetAll() {
        score.clear();
    }

    static Context<JDKResource> getContext() {
        return resource;
    }
}
