package jdk.crac;

/**
 * Some classes that could use logging are initialized early during the boot
 * and keeping the logger in static final field there could cause problems
 * (e.g. recursion when service-loading logger implementation).
 * Therefore, we isolate the logger into a subclass and initialize lazily.
 */
public class LoggerContainer {
    public static final System.Logger logger = System.getLogger("jdk.crac");

    public static void info(String msg) {
        logger.log(System.Logger.Level.INFO, msg);
    }

    public static void debug(String fmt, Object... params) {
        logger.log(System.Logger.Level.DEBUG, fmt, params);
    }

    private LoggerContainer() {}

    public static void error(String msg) {
        logger.log(System.Logger.Level.ERROR, msg);
    }

    public static void error(Throwable t, String msg) {
        logger.log(System.Logger.Level.ERROR, msg, t);
    }
}
