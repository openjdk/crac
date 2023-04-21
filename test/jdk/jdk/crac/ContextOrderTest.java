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

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;
import jdk.internal.crac.JDKResource;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;

import static jdk.crac.Core.getGlobalContext;
import static jdk.internal.crac.Core.*;
import static jdk.test.lib.Asserts.*;

/**
 * @test ContextOrderTest
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @run main ContextOrderTest
 */
public class ContextOrderTest {
    // prevents GC releasing the resources
    private static final List<Resource> rememberMe = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        testOrder();
        testCannotRegister();
        testThrowing();
    }

    private static void testOrder() throws CheckpointException, RestoreException {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, null, "regular1"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.NORMAL, "jdk-normal"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk-later"));
        getGlobalContext().register(new CreatingResource<>(recorder, null, "regular2", getJDKContext(), JDKResource.Priority.NORMAL));
        // this child should run as it has higher priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.NORMAL, "jdk-create", getJDKContext(), JDKResource.Priority.SEEDER_HOLDER));

        getGlobalContext().beforeCheckpoint(null);
        assertEquals("regular2-before", recorder.poll());
        assertEquals("regular1-before", recorder.poll());

        // The order of notifications with the same priority class is undefined
        List<String> normalPriority = Arrays.asList(recorder.poll(), recorder.poll(), recorder.poll());
        normalPriority.sort(String::compareTo);
        assertEquals("jdk-create-before", normalPriority.get(0));
        assertEquals("jdk-normal-before", normalPriority.get(1));
        assertEquals("regular2-child1-before", normalPriority.get(2));

        assertEquals("jdk-later-before", recorder.poll());
        assertEquals("jdk-create-child1-before", recorder.poll());
        assertNull(recorder.poll());

        getGlobalContext().afterRestore(null);
        assertEquals("jdk-create-child1-after", recorder.poll());
        assertEquals("jdk-later-after", recorder.poll());

        normalPriority = Arrays.asList(recorder.poll(), recorder.poll(), recorder.poll());
        normalPriority.sort(String::compareTo);
        assertEquals("jdk-create-after", normalPriority.get(0));
        assertEquals("jdk-normal-after", normalPriority.get(1));
        assertEquals("regular2-child1-after", normalPriority.get(2));

        assertEquals("regular1-after", recorder.poll());
        assertEquals("regular2-after", recorder.poll());
        assertNull(recorder.poll());

        // second checkpoint - whatever was registered in first afterRestore is now notified
        getGlobalContext().beforeCheckpoint(null);
        assertTrue(recorder.stream().anyMatch("jdk-create-child2-before"::equals));
        assertTrue(recorder.stream().anyMatch("regular2-child2-before"::equals));
    }

    private static void testCannotRegister() {
        var recorder = new LinkedList<String>();
        // cannot register into the same OrderedContext
        getGlobalContext().register(new CreatingResource<>(recorder, null, "regular", getGlobalContext(), null));
        // Cannot register with lower priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk-lower", getJDKContext(), JDKResource.Priority.NORMAL));
        // Cannot register with the same priority
        getJDKContext().register(new CreatingResource<>(recorder, JDKResource.Priority.NORMAL, "jdk-same", getJDKContext(), JDKResource.Priority.NORMAL));

        try {
            getGlobalContext().beforeCheckpoint(null);
            fail("Expected to throw CheckpointException");
        } catch (CheckpointException e) {
            assertEquals(3, e.getSuppressed().length);
        } finally {
            // Clear AbstractContextImpl.restoreQ
            try {
                getGlobalContext().afterRestore(null);
            } catch (RestoreException e) {
                // ignored
            }
            // Deregister all resources - we don't have a direct way to clear to contexts
            rememberMe.clear();
            System.gc();
        }
    }

    private static void testThrowing() throws RestoreException {
        var recorder = new LinkedList<String>();
        getGlobalContext().register(new MockResource(recorder, null, "regular1"));
        getGlobalContext().register(new ThrowingResource(null));
        getGlobalContext().register(new MockResource(recorder, null, "regular2"));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.NORMAL, "jdk1"));
        getJDKContext().register(new ThrowingResource(JDKResource.Priority.EPOLLSELECTOR));
        getJDKContext().register(new MockResource(recorder, JDKResource.Priority.SECURE_RANDOM, "jdk2"));

        try {
            getGlobalContext().beforeCheckpoint(null);
            fail("Expected to throw CheckpointException");
        } catch (CheckpointException e) {
            assertEquals(2, e.getSuppressed().length);
        }
        assertEquals("regular2-before", recorder.poll());
        assertEquals("regular1-before", recorder.poll());
        assertEquals("jdk1-before", recorder.poll());
        assertEquals("jdk2-before", recorder.poll());
        assertNull(recorder.poll());

        getGlobalContext().afterRestore(null);

        assertEquals("jdk2-after", recorder.poll());
        assertEquals("jdk1-after", recorder.poll());
        assertEquals("regular1-after", recorder.poll());
        assertEquals("regular2-after", recorder.poll());
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
            recorder.add(id + "-before");
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            recorder.add(id + "-after");
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

        private CreatingResource(List<String> recorder, Priority priority, String id, Context<R> childContext, Priority childPriority) {
            super(recorder, priority, id);
            this.childContext = childContext;
            this.childPriority = childPriority;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            super.beforeCheckpoint(context);
            //noinspection unchecked
            childContext.register((R) new MockResource(recorder, childPriority, id + "-child1"));
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            super.afterRestore(context);
            //noinspection unchecked
            childContext.register((R) new MockResource(recorder, childPriority, id + "-child2"));
        }
    }

    private static class ThrowingResource extends MockResource {
        private ThrowingResource(Priority priority) {
            super(null, priority, "throwing");
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            throw new RuntimeException();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            throw new RuntimeException();
        }
    }
}
