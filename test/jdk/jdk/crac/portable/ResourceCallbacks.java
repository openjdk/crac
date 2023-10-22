import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;

/**
 * Demonstrates resource callbacks (at the time of writing they are not entirely correct since
 * thread state is not restored), as well as restoration of static and non-static fields of
 * non-primitive types.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * // You must compile the class separately or restore will fail because CDS won't create aaaa
 * // the built-in class loaders
 * javac ResourceCallbacks.java
 *
 * java -XX:CREngine= -XX:CRaCCheckpointTo=cr ResourceCallbacks
 * // Output (VM messages omitted):
 * // Current time: 1697994964323
 * // Resource creation time: 1697994964389
 * // Time before checkpoint: 1697994964729
 * // Time after restore: 1697994965976
 *
 * // You have to specify the main class again for VM to find it and run it
 * java -XX:CREngine= -XX:CRaCRestoreFrom=cr ResourceCallbacks
 * // Output (VM messages omitted):
 * // Current time: 1697994969773
 * // Resource creation time: 1697994964389
 * // Resource was created 5384 ms ago and has been restored
 * }
 * </pre>
 */
public class ResourceCallbacks {
    private static MyResource resource;

    public static void main(String[] args) throws Exception {
        var startTime = System.currentTimeMillis();
        System.out.println("Current time: " + startTime);

        if (resource == null) {
            resource = new MyResource();
            Core.getGlobalContext().register(resource);
        }

        System.out.println("Resource creation time: " + resource.getCreationTime());
        if (resource.getCreationTime() >= startTime) {
            // The resource has been created during this run, so create a checkpoint
            Core.checkpointRestore();
        } else {
            // The resource has been restored
            var timeDiff = startTime - resource.getCreationTime();
            System.out.println("Resource was created " + timeDiff + " ms ago and has been restored");
        }
    }
}

class MyResource implements Resource {
    private String creationTimeStr = "the dawn of humanity";

    MyResource() {
        creationTimeStr = String.valueOf(System.currentTimeMillis());
    }

    public long getCreationTime() {
        return Long.parseLong(creationTimeStr);
    }

    @Override
    public void beforeCheckpoint(Context<?> context) {
        System.out.println("Time before checkpoint: " + System.currentTimeMillis());
    }

    @Override
    public void afterRestore(Context<?> context) {
        System.out.println("Time after restore: " + System.currentTimeMillis());
    }
}
