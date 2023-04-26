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
import jdk.crac.impl.OrderedContext;
import jdk.internal.crac.JDKResource;

import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;

import static jdk.crac.Core.getGlobalContext;
import static jdk.internal.crac.Core.*;
import static jdk.test.lib.Asserts.*;

/**
 * @test ContextOrderTest
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @modules java.base/jdk.crac.impl:+open
 * @run main/othervm -ea -XX:CREngine=simengine -XX:CRaCCheckpointTo=ignored ContextOrderTest
 */
public class ContextOrderTest {
    // prevents GC releasing the resources
    private static final List<Resource> rememberMe = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        testOrder();
        testCannotRegister();
        testThrowing();
        testRegisterToCompleted();
    }

    private static void testOrder() throws Exception {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, null, "regular1"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.NORMAL, "jdk-normal"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk-later"));
        getGlobalContext().register(new CreatingResource<>(recorder, null, "regular2", getJDKContext(), JDKResource.Priority.NORMAL));
        // this child should run as it has higher priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.NORMAL, "jdk-create", getJDKContext(), JDKResource.Priority.SEEDER_HOLDER));

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

        rememberMe.clear();
        System.gc();
    }

    private static void testCannotRegister() throws Exception {
        var recorder = new LinkedList<String>();
        // cannot register into the same OrderedContext
        getGlobalContext().register(new CreatingResource<>(recorder, null, "regular", getGlobalContext(), null));
        // Cannot register with lower priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk-lower", getJDKContext(), JDKResource.Priority.NORMAL));
        // Cannot register with the same priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.NORMAL, "jdk-same", getJDKContext(), JDKResource.Priority.NORMAL));

        try {
            Core.checkpointRestore();
            fail("Expected to throw CheckpointException");
        } catch (CheckpointException e) {
            assertEquals(3, e.getSuppressed().length);
        } finally {
            // Deregister all resources - we don't have a direct way to clear to contexts
            rememberMe.clear();
            System.gc();
        }
    }

    private static void testThrowing() throws Exception {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, null, "regular1"));
        getGlobalContext().register(new ThrowingResource(recorder, null, "throwing1"));
        getGlobalContext().register(new MockResource(recorder, null, "regular2"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.NORMAL, "jdk1"));
        getJDKContext().register(new ThrowingResource(recorder, JDKResource.Priority.EPOLLSELECTOR, "throwing2"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk2"));

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

        rememberMe.clear();
        System.gc();
    }

    // Similar to the test above but registers in context that is already done
    // rather than iterating through now. Also shows that registration adds
    // the resource to the context for the second attempt.
    private static void testRegisterToCompleted() throws Exception {
        var recorder = new LinkedList<String>();

        OrderedContext<Resource> c1 = new OrderedContext<>();
        OrderedContext<Resource> c2 = new OrderedContext<>();
        getGlobalContext().register(c1);
        getGlobalContext().register(c2);
        c2.register(new MockResource(recorder, null, "first"));
        c1.register(new CreatingResource<>(recorder, null, "second", c2, null));

        try {
            Core.checkpointRestore();
            fail("Expected checkpoint exception");
        } catch (CheckpointException e) {
            e.printStackTrace();
            assertEquals(1, e.getSuppressed().length);
        } finally {
            getGlobalContext().afterRestore(null);
        }
        recorder.clear();
        // second time we should succeed
        Core.checkpointRestore();
        assertEquals("second-child2-before", recorder.poll());
        assertEquals("second-child1-before", recorder.poll());
        assertEquals("first-before", recorder.poll());
        assertEquals("second-before", recorder.poll());

        assertEquals("second-after", recorder.poll());
        assertEquals("first-after", recorder.poll());
        assertEquals("second-child1-after", recorder.poll());
        assertEquals("second-child2-after", recorder.poll());
        assertNull(recorder.poll());
    }


    private static class MockResource implements JDKResource {
        protected final List<String> recorder;
        protected final Priority priority;
        protected final String id;

        private MockResource(List<String> recorder, Priority priority, String id) {
            rememberMe.add(this);
            this.recorder = recorder;
            this.priority = priority;
            this.id = id;
        }

        @Override
        public Priority getPriority() {
            return priority;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            ensureJDKContext(context);
            recorder.add(id + "-before");
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            ensureJDKContext(context);
            recorder.add(id + "-after");
        }

        private void ensureJDKContext(Context<? extends Resource> context) {
            if (priority != null && context != getJDKContext()) {
                throw new AssertionError(context.toString());
            }
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
        private final Priority childPriority;
        private final Context<R> childContext;
        private boolean first = true;

        private CreatingResource(List<String> recorder, Priority priority, String id, Context<R> childContext, Priority childPriority) {
            super(recorder, priority, id);
            this.childContext = childContext;
            this.childPriority = childPriority;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            super.beforeCheckpoint(context);
            if (first) {
                //noinspection unchecked
                childContext.register((R) new MockResource(recorder, childPriority, id + "-child1"));
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            super.afterRestore(context);
            if (first) {
                //noinspection unchecked
                childContext.register((R) new MockResource(recorder, childPriority, id + "-child2"));
            }
            first = false;
        }
    }

    private static class ThrowingResource extends MockResource {
        private ThrowingResource(List<String> recorder, Priority priority, String id) {
            super(recorder, priority, id);
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            super.beforeCheckpoint(context);
            throw new RuntimeException(id + "-before");
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            super.afterRestore(context);
            throw new RuntimeException(id + "-after");
        }
    }
}
