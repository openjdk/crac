import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;

/**
 * Demonstrates resource callbacks.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr ResourceCallbacks.java
 * > Resource creation time: 1700914709741
 * > Time before checkpoint: 1700914710112
 * > Time after restore: 1700914711393
 * > Resource creation time: 1700914709741
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Time after restore: 1700914712000
 * > Resource creation time: 1700914709741
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
