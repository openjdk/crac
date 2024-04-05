import jdk.crac.Core;

/**
 * Restoration of main thread's execution stack.
 *
 * <p> Invokes a recursive function to create a deep call stack and performs a
 * checkpoint there.
 *
 * <p> How to run:
 * <pre> {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr RecursionCounter.java
 * > -
 * > --
 * > ---
 * > # Checkpoint occurs
 * > ----
 * > ---
 * > --
 * > -
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > ----
 * > ---
 * > --
 * > -
 * }
 * </pre>
 */
public class RecursionCounter {
    private static void recurse(int depth, int maxDepth) throws Exception {
        final var depthStr = "-".repeat(depth);

        Thread.sleep(200);
        System.out.println(depthStr);

        if (depth < maxDepth) {
            recurse(depth + 1, maxDepth);
        } else {
            Core.checkpointRestore();
            System.out.println(depthStr + "-");
        }

        Thread.sleep(200);
        System.out.println(depthStr);
    }

    public static void main(String[] args) throws Exception {
        recurse(1, 10);
    }
}
