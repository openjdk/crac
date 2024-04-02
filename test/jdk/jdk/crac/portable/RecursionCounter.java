import jdk.crac.Core;

/**
 * Demonstrates that thread stack state can be restored.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr RecursionCounter.java
 * > -
 * > --
 * > -- 0
 * > --
 * > -
 * > -
 * > -- 1
 * > --
 * > -
 * > -
 * > --
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > -- 2
 * > -
 * > -
 * > --
 * > -- 3
 * > ...
 * }
 * </pre>
 */
public class RecursionCounter {
    private static final long NUM_ITERATIONS = 5;

    private static double work(long counter) {
        double result = 0;
        // Most likely, the checkpoint will happen while in this loop
        for (long innerCounter = 0; innerCounter < 90000000; innerCounter++) {
            result = innerCounter + result / counter;
        }
        return result;
    }

    private static void recurse(int depth, int maxDepth, long counter) throws Exception {
        var depthStr = "-".repeat(depth);
        var result = work(counter); // Spend some time calculating
        System.out.println(depthStr + (result % 2 == 0 ? " " : "  ")); // Blackhole for work() result

        if (depth < maxDepth) {
            recurse(depth + 1, maxDepth, counter);
        } else {
            if (counter == NUM_ITERATIONS / 2) {
                Core.checkpointRestore();
            }
            System.out.println(depthStr + " " + counter);
        }

        result = work(counter); // Spend some time calculating
        System.out.println(depthStr + (result % 2 == 0 ? " " : "  ")); // Blackhole for work() result
    }

    public static void main(String[] args) throws Exception {
        for (long counter = 0; counter < NUM_ITERATIONS; counter++) {
            recurse(1, 10, counter);
        }
    }
}
