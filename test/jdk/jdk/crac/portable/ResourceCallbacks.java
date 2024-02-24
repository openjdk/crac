import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;

/**
 * NOTE: currently this example DOES NOT WORK because restoration of the thread
 * initiating the checkpoint is not implemented (the thread runs native code at
 * the moment of checkpoint).
 *
 * Demonstrates resource callbacks.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * // You must compile the class separately or restore will fail because CDS won't create aaaa
 * // the built-in class loaders
 * javac ResourceCallbacks.java
 *
 * java -XX:CREngine= -XX:CRaCCheckpointTo=cr ResourceCallbacks
 * // Output:
 * // Resource creation time: 1700914709741
 * // Time before checkpoint: 1700914710112
 * // Time after restore: 1700914711393
 * // Resource creation time: 1700914709741
 *
 * // You have to specify the main class again for VM to find it and run it
 * java -XX:CREngine= -XX:CRaCRestoreFrom=cr ResourceCallbacks
 * // Output:
 * // Time after restore: 1700914712000
 * // Resource creation time: 1700914709741
 * }
 * </pre>
 */
public class ResourceCallbacks {
    public static void main(String[] args) throws Exception {
        var resource = new MyResource();
        Core.getGlobalContext().register(resource);
        Core.checkpointRestore();
    }
}

class MyResource implements Resource {
    long creationTime = System.currentTimeMillis();

    @Override
    public void beforeCheckpoint(Context<?> context) {
        System.out.println("Resource creation time: " + creationTime);
        System.out.println("Time before checkpoint: " + System.currentTimeMillis());
    }

    @Override
    public void afterRestore(Context<?> context) {
        System.out.println("Time after restore: " + System.currentTimeMillis());
        System.out.println("Resource creation time: " + creationTime);
    }
}
