/**
 * Demonstrates that a primitive static counter can be restored.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * // Shell 1
 * // You must compile the class separately or restore will fail because CDS won't create
 * // the built-in class loaders
 * javac InfiniteCounter.java
 * java -XX:CREngine= -XX:CRaCCheckpointTo=cr InfiniteCounter
 * // Output:
 * // 0
 * // 1
 * // 2
 * // ...
 *
 * // Shell 2
 * jcmd InfiniteCounter JDK.checkpoint
 *
 * // Shell 1
 * // <Press Ctrl-C to stop the previous Java process>
 * // You have to specify the main class again for VM to find it and run it
 * java -XX:CREngine= -XX:CRaCRestoreFrom=cr InfiniteCounter
 * // Output:
 * // 5
 * // 6
 * // ...
 * }
 * </pre>
 */
public class InfiniteCounter {
    private static int counter = 0;

    public static void main(String[] args) throws Exception {
        while (true) {
            System.out.println(counter++);
            Thread.sleep(1000);
        }
    }
}
