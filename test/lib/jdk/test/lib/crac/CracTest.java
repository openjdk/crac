package jdk.test.lib.crac;

import jdk.crac.Core;

import java.lang.reflect.*;
import java.util.Comparator;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.*;

/**
 * CRaC tests usually consists of two parts; the test started by JTreg through the 'run' tag
 * and subprocesses started by the test with various VM options. These are represented by the
 * {@link #test()} and {@link #exec()} methods.
 * As a convention the static main method invokes {@link #run(Class, String[]) CracTest.run(self.class, args)}
 * passing all arguments; this method instantiates the test (public no-arg constructor is needed),
 * populates fields annotated with {@link CracTestArg} and executes the {@link #test()} method.
 * The test method is expected to use {@link CracBuilder} to start another process using the same class
 * for {@link CracBuilder#main(Class)} and {@link #args()} for {@link CracBuilder#args(String...)}.
 */
public interface CracTest {

    /**
     * This method is called when JTReg invokes the test; it is supposed to start
     * another process (most often using CRaC VM options) and validate its behaviour.
     *
     * @throws Exception
     */
    void test() throws Exception;

    /**
     * This method is invoked in the subprocess; this is where you're likely to call
     * {@link Core#checkpointRestore()}.
     *
     * @throws Exception
     */
    void exec() throws Exception;

    class ArgsHolder {
        private static final String RUN_TEST = "__run_test__";
        private static String[] args;
    }

    /**
     * This method should be invoked from the public static void main(String[]) method.
     * @param testClass
     * @param args Arguments received in the main method.
     * @throws Exception
     */
    static void run(Class<? extends CracTest> testClass, String[] args) throws Exception {
        assertNotNull(args);
        int argsOffset = 0;
        if (args.length == 0 || !args[0].equals(ArgsHolder.RUN_TEST)) {
            String[] newArgs = new String[args.length + 1];
            newArgs[0] = ArgsHolder.RUN_TEST;
            System.arraycopy(args, 0, newArgs, 1, args.length);
            ArgsHolder.args = newArgs;
        } else {
            argsOffset = 1;
        }

        try {
            Constructor<? extends CracTest> ctor = testClass.getConstructor();
            CracTest testInstance = ctor.newInstance();
            Field[] argFields = getArgFields(testClass);
            for (int index = 0; index < argFields.length; index++) {
                Field f = argFields[index];
                assertFalse(Modifier.isFinal(f.getModifiers()), "@CracTestArg fields must not be final!");
                Class<?> t = f.getType();
                assertLessThan(index + argsOffset, args.length, "Not enough args for field " + f.getName() + "(" + index + "): have " + (args.length - argsOffset));
                String arg = args[index + argsOffset];
                Object value = arg;
                if (t == boolean.class || t == Boolean.class) {
                    assertTrue("true".equals(arg) || "false".equals(arg), "Boolean arg should be either 'true' or 'false', was: " + arg);
                    value = Boolean.parseBoolean(arg);
                } else if (t == int.class || t == Integer.class) {
                    try {
                        value = Integer.parseInt(arg);
                    } catch (NumberFormatException e) {
                        fail("Cannot parse argument '" + arg + "' as integer for @CracTestArg(" + index + ") " + f.getName());
                    }
                } else if (t == long.class || t == Long.class) {
                    try {
                        value = Long.parseLong(arg);
                    } catch (NumberFormatException e) {
                        fail("Cannot parse argument '" + arg + "' as long for @CracTestArg(" + index + ") " + f.getName());
                    }
                } else if (t.isEnum()) {
                    value = Enum.valueOf((Class<Enum>) t, arg);
                }
                f.setAccessible(true);
                f.set(testInstance, value);
            }
            if (argsOffset == 0) {
                testInstance.test();
            } else {
                testInstance.exec();
            }
        } catch (NoSuchMethodException e) {
            fail("Test class " + testClass.getName() + " is expected to have a public no-arg constructor");
        }
    }

    private static Field[] getArgFields(Class<? extends CracTest> testClass) {
        // TODO: check superclasses
        Field[] sortedFields = Stream.of(testClass.getDeclaredFields()).filter(f -> f.isAnnotationPresent(CracTestArg.class))
                .sorted(Comparator.comparingInt(f -> f.getAnnotation(CracTestArg.class).value()))
                .toArray(Field[]::new);
        if (sortedFields.length == 0) {
            return sortedFields;
        }
        for (int i = 0; i < sortedFields.length; ++i) {
            int index = sortedFields[i].getAnnotation(CracTestArg.class).value();
            assertGreaterThanOrEqual(index, 0);
            if (i == 0) {
                assertEquals(0, index, "@CracTestArg numbers should start with 0");
            }
            if (index < i) {
                fail("Duplicate @CracTestArg(" + index + "): both fields " + sortedFields[i - 1].getName() + " and " + sortedFields[i].getName());
            } else if (index > i) {
                fail("Gap in @CracTestArg indices: missing " + i + ", next is " + index);
            }
        }
        return sortedFields;
    }

    /**
     * Used as argument for {@link CracBuilder#args(String...)}.
     */
    static String[] args() {
        assertNotNull(ArgsHolder.args, "Args are null; are you trying to access them from test method?");
        return ArgsHolder.args;
    }
}

