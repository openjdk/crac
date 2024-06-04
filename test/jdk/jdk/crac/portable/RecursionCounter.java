/**
 * Demonstrates that thread stack state can be restored.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * // Shell 1
 * // You must compile the class separately or restore will fail because CDS won't create
 * // the built-in class loaders
 * javac RecursionCounter.java
 * java -XX:CREngine= -XX:CRaCCheckpointTo=cr RecursionCounter
 * // Output:
 * // -
 * // --
 * // ---
 * // --- 0
 * // ---
 * // --
 * // -
 * // -
 * // --
 * // ---
 * // --- 1
 * // ---
 * // ...
 *
 * // Shell 2
 * jcmd RecursionCounter JDK.checkpoint
 *
 * // Shell 1
 * // <Press Ctrl-C to stop the previous Java process>
 * // You have to specify the main class again for VM to find it and run it
 * java -XX:CREngine= -XX:CRaCRestoreFrom=cr RecursionCounter
 * // Output:
 * // --
 * // -
 * // -
 * // --
 * // ---
 * // --- 2
 * // ...
 * }
 * </pre>
 */
public class RecursionCounter {
    private static double work(long counter) {
        double result = 0;
        // Most likely, the checkpoint will happen while in this loop
        for (long innerCounter = 0; innerCounter < 90000000; innerCounter++) {
            result = innerCounter + result / counter;
        }
        return result;
    }

    private static void recurse(int depth, int maxDepth, long counter) {
        var depthStr = "-".repeat(depth);
        var result = work(counter); // Spend some time calculating
        System.out.println(depthStr + (result % 2 == 0 ? " " : "  ")); // Blackhole for work() result

        if (depth < maxDepth) {
            recurse(depth + 1, maxDepth, counter);
        } else {
            System.out.println(depthStr + " " + counter);
        }

        result = work(counter); // Spend some time calculating
        System.out.println(depthStr + (result % 2 == 0 ? " " : "  ")); // Blackhole for work() result
    }

    public static void main(String[] args) throws Exception {
        for (long counter = 0; counter < 10; counter++) {
            recurse(1, 10, counter);
        }
    }
}
