/**
 * Demonstrates that thread stack state can be restored.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * // Shell 1
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr RecursionCounter.java
 * > -
 * > --
 * > ---
 * > --- 0
 * > ---
 * > --
 * > -
 * > -
 * > --
 * > ---
 * > --- 1
 * > ---
 * > --
 * > ...
 *
 * // Shell 2 (specify "RecursionCounter" instead of the launcher in case of a
 * // pre-compiled version)
 * $ jcmd jdk.compiler/com.sun.tools.javac.launcher.Main JDK.checkpoint
 *
 * // Shell 1 (stop the previous Java process with Ctrl-C)
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > -
 * > -
 * > --
 * > ---
 * > --- 2
 * > ...
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
