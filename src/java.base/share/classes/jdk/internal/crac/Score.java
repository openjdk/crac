package jdk.internal.crac;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;

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

    public static native boolean isSupported();

    private static native boolean record(String[] metrics, double[] values);

    static synchronized void record() {
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
     * with the same {@code metric} overwrite previous value. If the engine does not support
     * recording metadata this is ignored.
     * On checkpoint the metrics are not reset; if that is desired invoke {@link #resetAll()}
     * manually.
     *
     * @param metric Name of the metric.
     * @param value Numeric value of the metric.
     */
    public static synchronized void setScore(String metric, double value) {
        score.put(metric, value);
    }

    public static synchronized void resetAll() {
        score.clear();
    }

    static Context<JDKResource> getContext() {
        return resource;
    }
}
