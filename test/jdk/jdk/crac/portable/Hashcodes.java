import java.util.HashMap;
import jdk.crac.Core;

/**
 * Restoration of VM-provided identity hash codes.
 *
 * <p> Creates objects with non-overriden hash code generation method and checks
 * that the hash codes stay the same across the C/R boundary.
 *
 * <p> How to run:
 * <pre> {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr Hashcodes.java
 * > Right test hash: 1204088028
 * > Right key 1 value
 * > Right key 2 value
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Right test hash: 1204088028
 * > Right key 1 value
 * > Right key 2 value
 * } </pre>
 */
public class Hashcodes {
    private static class Key {}

    public static void main(String[] args) throws Exception {
        final var hashtable = new HashMap<Key, String>();

        final Key key1 = new Key();
        final String expectedVal1 = "value 1";
        hashtable.put(key1, expectedVal1);

        final Key key2 = new Key();
        final String expectedVal2 = "value 2";
        hashtable.put(key2, expectedVal2);

        final var testKey = new Key();
        final int expectedTestKeyHash = testKey.hashCode();

        Core.checkpointRestore();

        boolean success = true;

        if (testKey.hashCode() == expectedTestKeyHash) {
            System.out.println("Right test hash: " + expectedTestKeyHash);
        } else {
            System.out.println("Wrong test hash: expected " + expectedTestKeyHash + ", got " + testKey.hashCode());
            success = false;
        }

        final String actualVal1 = hashtable.get(key1);
        if (actualVal1 == expectedVal1) {
            System.out.println("Right key 1 value");
        } else {
            System.out.println("Wrong key 1 value: expected " + expectedVal1 + ", got " + actualVal1);
            success = false;
        }

        final String actualVal2 = hashtable.get(key2);
        if (actualVal2 == expectedVal2) {
            System.out.println("Right key 2 value");
        } else {
            System.out.println("Wrong key 2 value: expected " + expectedVal2 + ", got " + actualVal2);
            success = false;
        }

        if (!success) {
            throw new IllegalStateException("Test failed");
        }
    }
}
