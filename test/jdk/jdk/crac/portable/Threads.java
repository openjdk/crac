import jdk.crac.Core;

import java.util.Random;

/**
 * Restoration of multiple threads.
 *
 * <p> The main threads starts concurrent threads, performs checkpoint-restore
 * and then interacts with the threads to ensure they are still properly
 * functioning.
 *
 * <p> How to run:
 * <pre> {@code
 * $ java -XX:CREngine= -XX:CRaCCheckpointTo=cr Threads.java
 * > My thread #2: in initial state
 * > ...
 * > My thread #9: in initial state
 * > Checkpointing
 * > ...
 *
 * $ java -XX:CREngine= -XX:CRaCRestoreFrom=cr
 * > Restored
 * > My thread #4: in initial state
 * > ...
 * > My thread #3: in initial state
 * > Changing states
 * > Changed state of My thread #0
 * > ...
 * > Changed state of My thread #9
 * > My thread #2: in new state (2)
 * > ...
 * > My thread #3: in new state (3)
 * }
 * </pre>
 */
public class Threads {
    static final Random random = new Random();

    // TODO replace with Thread.sleep() when C/R inside it is supported
    static String work(double period) {
        double x = random.nextDouble();
        for (double y = -period / 2; y < period / 2; y++) {
            x += (y / 2) % x;
        }
        return x % 2 == 0 ? "" : " "; // Blackhole
    }

    public static void main(String[] args) throws Exception {
        String blackhole;

        // Start some threads
        final var threads = new MyThread[10];
        for (int i = 0; i < threads.length; i++) {
            final var t = new MyThread(i);
            t.start();
            threads[i] = t;
        }

        // Waste some time to let the threads print something concurrently
        blackhole = work(70_000_000);

        System.out.println("Checkpointing" + blackhole);
        Core.checkpointRestore();
        System.out.println("Restored");

        // Waste some time to let the threads print something concurrently again
        blackhole = work(70_000_000);
        System.out.println("Changing states" + blackhole);

        // Change threads' state to check the objects we have are the actual
        // handles to the restored threads
        for (int i = 0; i < threads.length; i++) {
            final MyThread t = threads[i];
            t.strToPrint = "in new state (" + i + ")";
            System.out.println("Changed state of " + t.getName());
        }
        work(70_000_000);
    }
}

class MyThread extends Thread {
    String strToPrint = "in initial state";

    MyThread(int num) {
        super("My thread #" + num);
        setDaemon(true);
    }

    @Override
    public void run() {
      while (true) {
        Threads.work(50_000_000);
        System.out.println(getName() + ": " + strToPrint);
      }
    }
}
