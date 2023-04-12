package jdk.crac;

import jdk.internal.ref.CleanerFactory;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.lang.invoke.*;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;
import java.util.stream.Collectors;
import java.util.stream.StreamSupport;

/**
 * This class represents a RW-lock with extremely lightweight read-locking
 * and heavyweight write locking.
 * <p>
 * Readers should use the {@link #readLock()} and {@link #readUnlock()}; when
 * the writer lock is not locked these are similar to calling no-op methods.
 * The lock needs a declaration of all read-critical (top-level) methods;
 * contrary to the regular synchronization pattern we need to
 * <strong>acquire</strong> the read-lock <strong>inside</strong> the critical
 * section (at the beginning) and <strong>release</strong> it <strong>outside</strong>,
 * preferrably in the <code>finally</code> block.
 * Therefore, the code should be normally separated into wrapper and implementation method:
 * <hr><blockquote><pre>
 *   public void wrapper() {
 *       try {
 *           implementation();
 *       } finally {
 *           lock.readUnlock();
 *       }
 *   }
 *
 *   &#64;RCULock.Critical
 *   public void implementation() {
 *       lock.readLock();
 *       // critical code ...
 *   }
 * </pre></blockquote><hr>
 * In the example above the {@link RCULock.Critical} annotation would be used
 * in the {@link #forClasses(Class[])} factory method; alternatively you can
 * declare the methods (or their signatures) using one of the constructors.
 * <p>
 * There should be at most one writer invoking the {@link #synchronizeBegin()}
 * and {@link #synchronizeEnd()} pair. This operation requires all Java threads
 * to block in safepoint to inspect their stack; if the stack contains one of
 * the read-critical methods the thread acquires read-ownership even if
 * the actual <code>readLock()</code> invocation was a no-op.
 */
public class RCULock {
    private final ReentrantLock lock = new ReentrantLock(true);
    private final Condition beginCondition = lock.newCondition();
    private final Condition endCondition = lock.newCondition();
    private boolean synchronize;
    @SuppressWarnings("unused") // used in native code
    private long readerThreadsList;
    @SuppressWarnings("unused") // used in native code
    private long readCriticalMethods;
    private SwitchPoint lockSwitchPoint = new SwitchPoint();
    private SwitchPoint unlockSwitchPoint = new SwitchPoint();
    private volatile MethodHandle lockImpl;
    private volatile MethodHandle unlockImpl;

    static {
        initFieldOffsets();
    }

    private static native void initFieldOffsets();

    /**
     * Inspect the classes for all methods (including private ones) marked with
     * the {@link RCULock.Critical} annotation and create a lock that can protect
     * these methods with read lock.
     *
     * @param classes List of classes to be inspected.
     * @return New lock instance.
     */
    public static RCULock forClasses(Class<?>... classes) {
        ArrayList<Method> methods = new ArrayList<>();
        for (Class<?> cls : classes) {
            while (cls != null && cls != Object.class) {
                for (Method m : cls.getDeclaredMethods()) {
                    if (m.isAnnotationPresent(Critical.class)) {
                        methods.add(m);
                    }
                }
                cls = cls.getSuperclass();
            }
        }
        return new RCULock(methods);
    }

    /**
     * Create a lock that can protect selected methods with read-locking.
     *
     * @param readCriticalMethods List of methods with the read-critical section.
     */
    public RCULock(Iterable<Method> readCriticalMethods) {
        this(StreamSupport.stream(readCriticalMethods.spliterator(), false)
                .map(m -> m.getDeclaringClass().getName() + "." + m.getName() + "(" +
                Arrays.stream(m.getParameterTypes())
                        .map(Class::descriptorString).collect(Collectors.joining())
                + ")" + m.getReturnType().descriptorString()).toArray(String[]::new));
    }

    /**
     * Create a lock that can protect selected methods with read-locking.
     * <p>
     * The signatures follow the form
     * <pre>my.package.MyClass.foobar(another/package/SomeClass;Z)V</pre>
     *
     * @param readCriticalMethods List of signatures for methods invoked in the read-critical section.
     */
    public RCULock(String[] readCriticalMethods) {
        initSwitchPoints();
        //noinspection CapturingCleaner
        CleanerFactory.cleaner().register(this, this::destroy);
        Arrays.sort(readCriticalMethods);
        init(readCriticalMethods);
    }

    private native void init(String[] readCriticalMethods);

    private native void destroy();

    private void initSwitchPoints() {
        try {
            MethodType voidType = MethodType.methodType(void.class);
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            MethodHandle noop = lookup.findSpecial(RCULock.class, "noop", voidType, RCULock.class);
            MethodHandle readLockImpl = lookup.findSpecial(RCULock.class, "readLockImpl", voidType, RCULock.class);
            MethodHandle readUnlockImpl = lookup.findSpecial(RCULock.class, "readUnlockImpl", voidType, RCULock.class);
            lockSwitchPoint = new SwitchPoint();
            lockImpl = lockSwitchPoint.guardWithTest(noop, readLockImpl);
            unlockSwitchPoint = new SwitchPoint();
            unlockImpl = lockSwitchPoint.guardWithTest(noop, readUnlockImpl);
        } catch (NoSuchMethodException | IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    private void noop() {
    }

    /**
     * Acquire the read-lock. This method <strong>must</strong> be called from
     * within the read-critical method declared when this lock is created.
     *
     * @implNote It is not possible to guarantee that the thread would jump into
     * the critical section right after <code>readLock()</code> without being spotted
     * in-between; therefore the implementation works even if <code>readLock()</code>
     * is called after the synchronizer thread finds it in the critical section.
     */
    public void readLock() {
        try {
            lockImpl.invokeExact(this);
        } catch (Throwable e) {
            throw new RuntimeException(e);
        }
    }

    private void readLockImpl() {
        lock.lock();
        try {
            // In case that we have been caught in the critical section
            // before calling readLock() this thread was added to the list
            // but won't be removed as we will block.
            // After continuing in the critical section any subsequent
            // synchronizeBegin will catch us in again, so we don't need to
            // do anything.
            removeThread();
            // The lock is released as we are waiting for beginCondition
            while (synchronize) {
                endCondition.awaitUninterruptibly();
            }
        } finally {
            lock.unlock();
        }
    }

    /**
     * Release the read-lock. This method must be called <strong>outside</strong>
     * the critical section, preferably in a <code>finally</code> block.
     */
    public void readUnlock() {
        try {
            unlockImpl.invokeExact(this);
        } catch (Throwable e) {
            throw new RuntimeException(e);
        }
    }

    private void readUnlockImpl() {
        lock.lock();
        try {
            removeThread();
            beginCondition.signal();
        } finally {
            lock.unlock();
        }
    }

    private native void removeThread();

    /**
     * Acquire write-lock. It is illegal to call this method concurrently,
     * if you require to arbitrate the synchronizer thread use an external lock.
     * The lock should be later released using {@link #synchronizeEnd()}.
     */
    public void synchronizeBegin() {
        lock.lock();
        if (synchronize) {
            lock.unlock();
            throw new IllegalMonitorStateException("Concurrent synchronization or bug?");
        }
        synchronize = true;
        SwitchPoint.invalidateAll(new SwitchPoint[]{ lockSwitchPoint, unlockSwitchPoint });
        synchronizeThreads();
        while (hasReaderThreads()) {
            beginCondition.awaitUninterruptibly();
        }
        // No unlocking here
    }

    public native boolean hasReaderThreads();

    public native void synchronizeThreads();

    /**
     * Release write-lock. This method can be called only by the thread that previously
     * successfully called {@link #synchronizeBegin()}.
     */
    public void synchronizeEnd() {
        if (!lock.isHeldByCurrentThread()) {
            throw new IllegalMonitorStateException("The lock is not held by ");
        }
        initSwitchPoints();
        synchronize = false;
        endCondition.signalAll();
        lock.unlock();
    }

    /**
     * Marks read-critical methods. This is used in combination with
     * the {@link #forClasses(Class[])} factory method.
     */
    @Target(ElementType.METHOD)
    @Retention(RetentionPolicy.RUNTIME)
    public @interface Critical {
    }
}
