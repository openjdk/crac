/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import jdk.crac.*;
import jdk.crac.impl.BlockingOrderedContext;
import jdk.crac.impl.OrderedContext;
import jdk.internal.crac.JDKResource;
import jdk.test.lib.Utils;

import java.lang.reflect.Method;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import static jdk.crac.Core.getGlobalContext;
import static jdk.internal.crac.Core.*;
import static jdk.internal.crac.Core.Priority.*;
import static jdk.test.lib.Asserts.*;

/**
 * @test ContextOrderTest
 * @requires os.family == "linux"
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @modules java.base/jdk.crac.impl:+open
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest testOrder
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest testRegisterBlocks
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest testThrowing
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest testRegisterToCompleted
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest testRegisterFromOtherThread
 */
public class ContextOrderTest {
    // prevents GC releasing the resources
    private static final List<Resource> rememberMe = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        System.setProperty("java.util.logging.config.file", Utils.TEST_SRC + "/logging.properties");

        Method m = ContextOrderTest.class.getDeclaredMethod(args[0]);
        m.invoke(null);
    }

    private static void testOrder() throws Exception {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, "regular1"));
        NORMAL.getContext().register(new MockResource(recorder, "jdk-normal"));
        SECURE_RANDOM.getContext().register(new MockResource(recorder, "jdk-later"));
        getGlobalContext().register(new CreatingResource<>(recorder, "regular2", NORMAL.getContext()));
        // this child should run as it has higher priority
        NORMAL.getContext().register(new CreatingResource<>(recorder, "jdk-create", SEEDER_HOLDER.getContext()));

        Core.checkpointRestore();

        assertEquals("regular2-before", recorder.poll());
        assertEquals("regular1-before", recorder.poll());

        assertEquals("regular2-child1-before", recorder.poll());
        assertEquals("jdk-create-before", recorder.poll());
        assertEquals("jdk-normal-before", recorder.poll());

        assertEquals("jdk-later-before", recorder.poll());
        assertEquals("jdk-create-child1-before", recorder.poll());
        // restore
        assertEquals("jdk-create-child1-after", recorder.poll());
        assertEquals("jdk-later-after", recorder.poll());

        assertEquals("jdk-normal-after", recorder.poll());
        assertEquals("jdk-create-after", recorder.poll());
        assertEquals("regular2-child1-after", recorder.poll());

        assertEquals("regular1-after", recorder.poll());
        assertEquals("regular2-after", recorder.poll());
        assertNull(recorder.poll());

        // second checkpoint - whatever was registered in first afterRestore is now notified
        Core.checkpointRestore();
        assertTrue(recorder.stream().anyMatch("jdk-create-child2-before"::equals));
        assertTrue(recorder.stream().anyMatch("regular2-child2-before"::equals));
    }

    private static void testRegisterBlocks() throws Exception {
        var recorder = new LinkedList<String>();
        BlockingOrderedContext<Resource> blockingCtx = new BlockingOrderedContext();
        getGlobalContext().register(blockingCtx);
        // blocks register into the same OrderedContext
        blockingCtx.register(new CreatingResource<>(recorder, "regular", blockingCtx));
        testWaiting();

        BlockingOrderedContext<Resource> blockingCtx1 = new BlockingOrderedContext();
        getGlobalContext().register(blockingCtx1);
        BlockingOrderedContext<Resource> blockingCtx2 = new BlockingOrderedContext();
        getGlobalContext().register(blockingCtx2);
        // blocks registering to the context done notifications
        blockingCtx1.register(new CreatingResource<>(recorder, "jdk-lower", blockingCtx2));
        testWaiting();

        // blocks registering with the same priority
        NORMAL.getContext().register(new CreatingResource<>(recorder, "jdk-same", NORMAL.getContext()));
        testWaiting();
    }

    private static void testWaiting() throws InterruptedException {
        AtomicReference<Throwable> exceptionHolder = new AtomicReference<>();
        assertWaits(() -> {
            try {
                Core.checkpointRestore();
            } catch (Exception e) {
                exceptionHolder.set(e);
            }
        }, null, "waitWhileCheckpointIsInProgress");
        assertNotNull(exceptionHolder.get());
        exceptionHolder.get().printStackTrace();
    }

    private static void assertWaits(Runnable runnable, String cls, String method) throws InterruptedException {
        Thread thread = new Thread(runnable);
        thread.start();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        for (;;) {
            if (thread.getState() == Thread.State.WAITING) {
                for (var ste : thread.getStackTrace()) {
                    if ((cls == null || cls.equals(ste.getClassName()) || ste.getClassName().endsWith("." + cls)) &&
                            (method == null || method.equals(ste.getMethodName()))) {
                        // It should be sufficient to interrupt the code once; if any Resource
                        // clears the flag without rethrowing it is a bug.
                        thread.interrupt();
                        thread.join(TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime()));
                        if (thread.getState() == Thread.State.WAITING) {
                            for (var ste2 : thread.getStackTrace()) {
                                System.out.println(ste2);
                            }
                        }
                        assertFalse(thread.isAlive());
                        return;
                    }
                }
            } else if (thread.getState() == Thread.State.TERMINATED) {
                fail("Thread completed without waiting");
            }
            if (System.nanoTime() < deadline) {
                //noinspection BusyWait
                Thread.sleep(50);
            } else {
                fail("Timed out waiting for thread to get waiting in " + cls + "." + method);
            }
        }
    }

    private static void testThrowing() throws Exception {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, "regular1"));
        getGlobalContext().register(new ThrowingResource(recorder, "throwing1"));
        getGlobalContext().register(new MockResource(recorder, "regular2"));
        NORMAL.getContext().register(new MockResource(recorder, "jdk1"));
        EPOLLSELECTOR.getContext().register(new ThrowingResource(recorder, "throwing2"));
        SECURE_RANDOM.getContext().register(new MockResource(recorder, "jdk2"));

        try {
            Core.checkpointRestore();
            fail("Expected to throw CheckpointException");
        } catch (CheckpointException e) {
            assertEquals(4, e.getSuppressed().length);
        }
        assertEquals("regular2-before", recorder.poll());
        assertEquals("throwing1-before", recorder.poll());
        assertEquals("regular1-before", recorder.poll());
        assertEquals("jdk1-before", recorder.poll());
        assertEquals("throwing2-before", recorder.poll());
        assertEquals("jdk2-before", recorder.poll());

        assertEquals("jdk2-after", recorder.poll());
        assertEquals("throwing2-after", recorder.poll());
        assertEquals("jdk1-after", recorder.poll());
        assertEquals("regular1-after", recorder.poll());
        assertEquals("throwing1-after", recorder.poll());
        assertEquals("regular2-after", recorder.poll());
        assertNull(recorder.poll());
    }

    static class NamedOrderedContext<R extends Resource> extends BlockingOrderedContext {
        private final String name;
        NamedOrderedContext(String name) {
            this.name = name;
        }
    }

    // Similar to the test above but registers in context that is already done
    // rather than iterating through now.
    private static void testRegisterToCompleted() throws Exception {
        var recorder = new LinkedList<String>();

        OrderedContext<Resource> c1 = new NamedOrderedContext<>("C1");
        OrderedContext<Resource> c2 = new NamedOrderedContext<>("C2");
        getGlobalContext().register(c1);
        getGlobalContext().register(c2);
        c2.register(new MockResource(recorder, "first"));
        // Logically there's nothing that prevents to register into C2 during C1.<resource>.afterRestore
        // but the implementation of C1 does not know that we're already after C/R and still blocks
        // any registrations.
        c1.register(new CreatingResource<>(recorder, "second", c2));

        // This is supposed to end up with a deadlock. Even though it would block first
        // for -child1 and then for -child2 the first time we interrupt the thread it will
        // unblock and won't block any further.
        testWaiting();

        // Since we have interrupted the registration no other resource was registered
        // so there's no point in testing anything here.
    }

    // registering from lower priority resource to higher priority shouldn't block
    // even in another thread
    private static void testRegisterFromOtherThread() throws RestoreException, CheckpointException {
        var recorder = new LinkedList<String>();
        NORMAL.getContext().register(new MockResource(recorder, "normal") {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
                super.beforeCheckpoint(context);
                Thread thread = new Thread(() -> {
                    CLEANERS.getContext().register(new MockResource(recorder, "child"));
                }, "registrar");
                thread.start();
                thread.join();
            }
        });

        Core.checkpointRestore();
        assertEquals("normal-before", recorder.poll());
        assertEquals("child-before", recorder.poll());
        assertEquals("child-after", recorder.poll());
        assertEquals("normal-after", recorder.poll());
    }

    private static class MockResource implements JDKResource {
        protected final List<String> recorder;
        protected final String id;

        private MockResource(List<String> recorder, String id) {
            rememberMe.add(this);
            this.recorder = recorder;
            this.id = id;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            recorder.add(id + "-before");
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            recorder.add(id + "-after");
        }

        @Override
        public String toString() {
            return getClass().getSimpleName() + ":" + id;
        }
    }

    // While normally resources should not directly register other resources it is possible
    // that running it will trigger (static) initialization of a class and that registers
    // a new resource. It is not legal to register a user resource, but for JDK resources
    // we can make an exception since it does not conflict with the general order (JDK resources
    // are notified after user resources).
    private static class CreatingResource<R extends Resource> extends MockResource {
        private final Context<R> childContext;
        private boolean first = true;

        private CreatingResource(List<String> recorder, String id, Context<R> childContext) {
            super(recorder, id);
            this.childContext = childContext;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            super.beforeCheckpoint(context);
            if (first) {
                //noinspection unchecked
                childContext.register((R) new MockResource(recorder, id + "-child1"));
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            super.afterRestore(context);
            if (first) {
                //noinspection unchecked
                childContext.register((R) new MockResource(recorder, id + "-child2"));
            }
            first = false;
        }
    }

    private static class ThrowingResource extends MockResource {
        private ThrowingResource(List<String> recorder, String id) {
            super(recorder, id);
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            super.beforeCheckpoint(context);
            throw new RuntimeException(id + "-before");
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            super.afterRestore(context);
            throw new RuntimeException(id + "-after");
        }
    }
}
