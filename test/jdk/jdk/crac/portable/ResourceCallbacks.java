import java.text.SimpleDateFormat;
import java.util.Date;

import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;

/**
 * Restoration of CRaC resources.
 *
 * <p> Registers a resource that records its creation time and performs
 * checkpoint-restore to demonstrate that the callbacks of the resource are
 * executed as expected and the state of the resource is restored.
 *
 * <p> How to run:
 * <pre> {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr ResourceCallbacks.java
 * > Resource creation time: 13:16:06:486 05.04.2024
 * > Time before checkpoint: 13:16:06:670 05.04.2024
 * > # Checkpoint occurs
 * > Resource creation time: 13:16:06:486 05.04.2024
 * > Time after restore:     13:16:07:583 05.04.2024
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Resource creation time: 13:16:06:486 05.04.2024
 * > Time after restore:     13:16:40:875 05.04.2024
 * }
 * </pre>
 */
public class ResourceCallbacks {
    public static void main(String[] args) throws Exception {
        final var resource = new MyResource();
        Core.getGlobalContext().register(resource);
        Core.checkpointRestore();
    }
}

class MyResource implements Resource {
    final long creationTime = System.currentTimeMillis();

    private static String timeToString(long millis) {
        return (new SimpleDateFormat("HH:mm:ss:SSS dd.MM.yyyy")).format(new Date(millis));
    }

    @Override
    public void beforeCheckpoint(Context<?> context) {
        System.out.println("Resource creation time: " + timeToString(creationTime));
        System.out.println("Time before checkpoint: " + timeToString(System.currentTimeMillis()));
    }

    @Override
    public void afterRestore(Context<?> context) {
        System.out.println("Resource creation time: " + timeToString(creationTime));
        System.out.println("Time after restore:     " + timeToString(System.currentTimeMillis()));
    }
}
