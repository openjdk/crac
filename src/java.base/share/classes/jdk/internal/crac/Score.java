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

import java.util.*;

/**
 * CRaC Engine can support storing additional metadata about the image. This
 * can help the infrastructure to further refine the set of feasible images
 * (constrained by CPU architecture and features) and select the image that is
 * expected to perform best.
 * <p>
 * All methods of this class can be safely called even if the CRaC Engine in
 * use does not support storing metadata. In such case score will be ignored.
 */
public class Score {
    /**
     * Values of metrics provided by the user and Java classes of JDK.
     * <p>
     * JVM provides some metrics as well - they are not stored here,
     * {@link #getJvmScore()} is used to retrieve those.
     */
    private static final Map<String, Double> score = new HashMap<>();
    private static final Set<Runnable> scoreProviders = Collections.newSetFromMap(new WeakHashMap<>());

    private Score() {
    }

    /**
     * Returns {@code true} if the CRaC Engine in use supports score recording.
     *
     * @return {@code true} if the CRaC Engine in use supports score recording.
     */
    public static native boolean isSupported();

    /**
     * Records a value to be stored in the image metadata on checkpoint.
     * Repeated invocations with the same {@code metric} overwrite previous
     * value.
     * <p>
     * On checkpoint the metrics are not reset; if that is desired use
     * {@link #removeScore(String)}.
     *
     * @implNote Metrics {@code java.*}, {@code jdk.*}, {@code sun.*},
     * {@code vm.*} are reserved to be set by the JDK itself.
     *
     * @param metric Name of the metric.
     * @param value  Numeric value of the metric.
     */
    public static synchronized void setScore(String metric, double value) {
        score.put(metric, value);
    }

    /**
     * Drops the value of the specified metric, if one is recorded.
     *
     * @param metric Name of the metric.
     */
    public static synchronized void removeScore(String metric) {
        score.remove(metric);
    }

    /**
     * Adds a runnable which will be invoked each time just before a score is
     * read to provide an up-to-date value of some metrics.
     * <p>
     * The provider is expected to add or update the score by invoking
     * {@link #setScore(String, double)}, possibly multiple times.
     * <p>
     * The provider is recorded through a weak reference. The caller is
     * responsible for storing a strong reference while the provider functions.
     * <p>
     * The order in which the providers are called is indeterminate and can
     * change.
     *
     * @param provider The score provider to be recorded.
     */
    public static synchronized void addScoreProvider(Runnable provider) {
        scoreProviders.add(provider);
    }

    /**
     * Returns a snapshot of the recorded score. Future changes to the score
     * will not be reflected in the snapshot and vice versa.
     *
     * @return snapshot of the recorded score.
     */
    public static Map<String, Double> getScore() {
        // The same JVM after Java order is used for the actual score recording. It matters when there are collisions,
        // i.e. when custom metrics use reserved names. Technically that would be a user's fault but better to be
        // consistent.
        final Map<String, Double> scoreSnapshot = getJavaScore();
        final var jvmScore = getJvmScore();
        for (final var pair : jvmScore) {
            scoreSnapshot.put((String) pair[0], (Double) pair[1]);
        }
        return scoreSnapshot;
    }

    private static Map<String, Double> getJavaScore() {
        // Take a snapshot in case a provider adds new providers
        final Set<Runnable> providersSnapshot;
        synchronized (Score.class) {
            providersSnapshot = new HashSet<>(scoreProviders);
        }
        for (final var provider : providersSnapshot) {
            provider.run();
        }

        final Map<String, Double> scoreSnapshot;
        synchronized (Score.class) {
            scoreSnapshot = new HashMap<>(score);
        }
        return scoreSnapshot;
    }

    /**
     * @return Pairs of {@link String} and {@link Double}.
     */
    private static native Object[][] getJvmScore();

    private static final JDKResource resource = new JDKResource() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            recordJavaScore(); // JVM score will be recorded by JVM itself
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }
    };

    static {
        Core.Priority.NORMAL.getContext().register(resource);
    }

    private static void recordJavaScore() {
        final var scoreSnapshot = getJavaScore();
        final var metrics = new String[scoreSnapshot.size()];
        final var values = new double[scoreSnapshot.size()];
        int i = 0;
        for (var e : scoreSnapshot.entrySet()) {
            metrics[i] = e.getKey();
            values[i] = e.getValue();
            ++i;
        }
        recordJavaScore(metrics, values);
    }

    private static native void recordJavaScore(String[] metrics, double[] values);
}
