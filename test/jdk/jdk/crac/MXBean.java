import org.crac.*;
import org.crac.management.*;

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.lang.management.ManagementFactory;
import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;

/**
 * @test
 * @library /test/lib
 * @run main MXBean
 */
public class MXBean {
    static final long TIME_TOLERANCE = 10_000; // ms

    static class Test {
        public static void main(String[] args) throws CheckpointException, RestoreException {
            CRaCMXBean cracMXBean = CRaCMXBean.getCRaCMXBean();

            Core.checkpointRestore();

            System.out.println("UptimeSinceRestore " + cracMXBean.getUptimeSinceRestore());

            long restoreTime = cracMXBean.getRestoreTime();
            System.out.println("RestoreTime " + restoreTime + " " +
                DateTimeFormatter.ofPattern("E dd LLL yyyy HH:mm:ss.n").format(
                    Instant.ofEpochMilli(restoreTime)
                        .atZone(ZoneId.systemDefault())));
        }
    }

    public static void main(String[] args) {
        long start = System.currentTimeMillis();

        OutputAnalyzer output;
        try {
            output = ProcessTools.executeTestJvm(
                "-XX:CREngine=simengine", "-XX:CRaCCheckpointTo=./cr",
                "MXBean$Test");
        } catch (Exception e) {
            throw new RuntimeException(e);
        }

        output.shouldHaveExitValue(0);

        long restoreUptime = Long.parseLong(output.firstMatch("UptimeSinceRestore ([0-9-]+)", 1));
        if (restoreUptime < 0 || TIME_TOLERANCE < restoreUptime) {
            throw new Error("bad UptimeSinceRestore: " + restoreUptime);
        }

        long restoreTime = Long.parseLong(output.firstMatch("RestoreTime ([0-9-]+)", 1));
        restoreTime -= start;

        if (restoreTime < -TIME_TOLERANCE || TIME_TOLERANCE < restoreTime) {
            throw new Error("bad RestoreTime: " + restoreTime);
        }
    }
}
