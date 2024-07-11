import jdk.crac.Core;

/**
 * Restoration of monitor states.
 *
 * <p> Creates a monitor shared between two threads. The main thread enters the
 * monitor and initiates C/R while the secondary thread is blocked. After the
 * restoration it remains blocked until the main thread exits the monitor.
 *
 * <p> How to run:
 * <pre> {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr Monitors.java
 * > Thread[#1,main,5,main]: before synchronized
 * > Thread[#1,main,5,main]: inside synchronized
 * > Thread[#25,secondary,5,main]: before synchronized
 * > Thread[#1,main,5,main]: checkpointing
 * > # Checkpoint occurs
 * > # ... (the rest of the output omitted)
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Thread[#1,main,5,main]: restored
 * > Thread[#1,main,5,main]: deeply synchronized
 * > Thread[#1,main,5,main]: leaving synchronized
 * > Thread[#1,main,5,main]: after synchronized
 * > Thread[#25,secondary,5,main]: inside synchronized
 * > Thread[#25,secondary,5,main]: deeply synchronized
 * > Thread[#25,secondary,5,main]: leaving synchronized
 * > Thread[#25,secondary,5,main]: after synchronized
 * } </pre>
 */
public class Monitors {
    private static final Object sharedMonitor = new Object();

    private static void primaryThreadEntry() throws Exception {
        final Thread secondary = new Thread(Monitors::secondaryThreadEntry, "secondary");

        final String threadStr = Thread.currentThread().toString();
        System.out.println(threadStr + ": before synchronized");
        synchronized(sharedMonitor) {
            System.out.println(threadStr + ": inside synchronized");
            secondary.start();
            synchronized(sharedMonitor) {
                System.out.println(threadStr + ": checkpointing");
                Core.checkpointRestore();
                System.out.println(threadStr + ": restored");
                Thread.sleep(1000); // Let the secondary thread wake up
                synchronized(sharedMonitor) {
                    System.out.println(threadStr + ": deeply synchronized");
                }
            }
            System.out.println(threadStr + ": leaving synchronized");
        }
        System.out.println(threadStr + ": after synchronized");

        // TODO currently when the restored main thread dies the whole VM dies
        secondary.join();
    }

    private static void secondaryThreadEntry() {
        final String threadStr = Thread.currentThread().toString();
        System.out.println(threadStr + ": before synchronized");
        synchronized(sharedMonitor) {
            System.out.println(threadStr + ": inside synchronized");
            synchronized(sharedMonitor) {
                synchronized(sharedMonitor) {
                    synchronized(sharedMonitor) {
                        System.out.println(threadStr + ": deeply synchronized");
                    }
                }
            }
            System.out.println(threadStr + ": leaving synchronized");
        }
        System.out.println(threadStr + ": after synchronized");
    }

    public static void main(String[] args) throws Exception {
        primaryThreadEntry();
    }
}
