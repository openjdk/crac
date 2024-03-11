import java.util.HashMap;
import jdk.crac.Core;

/**
 * Demonstrates restoration of identity hash codes.
 * <p>
 * How to run:
 * <pre>
 * {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr Hashcodes.java
 * > Right x hash: 1204088028
 * > Right key1 value
 * > Right key2 value
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Right x hash: 1204088028
 * > Right key1 value
 * > Right key2 value
 * }
 * </pre>
 */
public class Hashcodes {
    private static class Key {}

    public static void main(String[] args) throws Exception {
        var hashtable = new HashMap<Key, String>();

        Key key1 = new Key();
        String val1 = "value 1";
        hashtable.put(key1, val1);

        Key key2 = new Key();
        String val2 = "value 2";
        hashtable.put(key2, val2);

        var x = new Key();
        int xHash = x.hashCode();

        Core.checkpointRestore();

        if (x.hashCode() != xHash) {
            System.out.println("WRONG x hash: expected " + xHash + ", got " + x.hashCode());
        } else {
            System.out.println("Right x hash: " + xHash);
        }

        String valFromKey1 = hashtable.get(key1);
        if (valFromKey1 != val1) {
            System.out.println("WRONG key1 value");
        } else {
            System.out.println("Right key1 value");
        }

        String valFromKey2 = hashtable.get(key2);
        if (valFromKey2 != val2) {
            System.out.println("WRONG key2 value");
        } else {
            System.out.println("Right key2 value");
        }
    }
}
