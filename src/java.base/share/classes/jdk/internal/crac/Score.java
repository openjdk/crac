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
 * use does not support storing metadata. In such case scores will be ignored.
 */
public class Score {
    private static final Map<String, Double> scores = new HashMap<>();
    private static final Set<Runnable> scoreProviders = Collections.newSetFromMap(new WeakHashMap<>());
    // CDS assert fails during VM initialization if this is a lambda
    private static final Runnable jvmScoreProvider = new Runnable() {
        @Override
        public void run() {
            final var jvmScores = getJvmScores();
            for (var i = 0; i < jvmScores.length; i++) {
                setScore((String) jvmScores[2 * i], (Double) jvmScores[2 * i + 1]);
            }
        }
    };

    static {
        addScoreProvider(jvmScoreProvider);
    }

    private static final Context<JDKResource> context = new Context<>() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            final var scoresSnapshot = getScores();
            final var metrics = new String[scoresSnapshot.size()];
            final var values = new double[scoresSnapshot.size()];
            int i = 0;
            for (var e : scoresSnapshot.entrySet()) {
                metrics[i] = e.getKey();
                values[i] = e.getValue();
                ++i;
            }
            record(metrics, values);
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }

        @Override
        public void register(JDKResource resource) {
            throw new UnsupportedOperationException("Score is a singleton context");
        }
    };

    private Score() {
    }

    /**
     * Returns {@code true} if the CRaC Engine in use supports score recording.
     *
     * @return {@code true} if the CRaC Engine in use supports score recording.
     */
    public static native boolean isSupported();

    /**
     * Records a score to be stored in the image metadata on checkpoint.
     * Repeated invocations with the same {@code metric} overwrite the previous
     * value.
     * <p>
     * On checkpoint the scores are not reset; if that is desired use
     * {@link #removeScore(String)}.
     *
     * @param metric Name of the metric.
     * @param value  Numeric value of the metric.
     * @implNote Metrics {@code java.*}, {@code jdk.*}, {@code sun.*},
     * {@code vm.*} are reserved to be set by the JDK itself.
     */
    public static synchronized void setScore(String metric, double value) {
        scores.put(metric, value);
    }

    /**
     * Drops the value of the specified metric, if one is recorded.
     *
     * @param metric Name of the metric.
     * @return {@code true} if the metric was present.
     */
    public static synchronized boolean removeScore(String metric) {
        return scores.remove(metric) != null;
    }

    /**
     * Adds a runnable which will be invoked each time just before scores are
     * read to provide up-to-date scores.
     * <p>
     * The provider is expected to add or update the scores by invoking
     * {@link #setScore(String, double)}, possibly multiple times.
     * <p>
     * The provider is recorded through a weak reference. The caller is
     * responsible for storing a strong reference while the provider functions.
     * <p>
     * The order in which the providers are called is indeterminate and can
     * change, they may even be called concurrently.
     * <p>
     * If a provider throws an {@link Exception} it is ignored and the score
     * computation proceeds. Other kinds of {@link Throwable} interrupt the
     * score computation and are propagated.
     *
     * @param provider The score provider to be recorded.
     */
    public static synchronized void addScoreProvider(Runnable provider) {
        scoreProviders.add(provider);
    }

    /**
     * Returns a snapshot of the recorded scores. Future changes to the scores
     * will not be reflected in the snapshot and vice versa.
     *
     * @return current snapshot of the recorded scores.
     */
    public static Map<String, Double> getScores() {
        // Take a snapshot in case a provider adds new providers
        final Set<Runnable> providersSnapshot;
        synchronized (Score.class) {
            providersSnapshot = new HashSet<>(scoreProviders);
        }
        for (final var provider : providersSnapshot) {
            try {
                provider.run();
            } catch (Exception _) {
            }
        }

        final Map<String, Double> scoresSnapshot;
        synchronized (Score.class) {
            scoresSnapshot = new HashMap<>(scores);
        }
        return scoresSnapshot;
    }

    private static native Object[] getJvmScores();

    private static native void record(String[] metrics, double[] values);

    static Context<JDKResource> getContext() {
        return context;
    }
}
