/*
 * Copyright (c) 2014, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
package jdk.internal.jimage;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;

/**
 * @implNote This class needs to maintain JDK 8 source compatibility.
 *
 * It is used internally in the JDK to implement jimage/jrtfs access,
 * but also compiled and delivered as part of the jrtfs.jar to support access
 * to the jimage file provided by the shipped JDK by tools running on JDK 8.
 */
@SuppressWarnings("removal")
class NativeImageBuffer {
    private static Object nativeInitResource;

    static {
        loadNativeLibrary();
        registerIfCRaCPresent();
    }

    private static void loadNativeLibrary() {
        java.security.AccessController.doPrivileged(
                new java.security.PrivilegedAction<Void>() {
                    public Void run() {
                        System.loadLibrary("jimage");
                        return null;
                    }
                });
    }

    // Since this class must be compatible with JDK 8 and any non-CRaC JDK due
    // to being part of jrtfs.jar we must register this to CRaC via reflection.
    private static void registerIfCRaCPresent() {
        try {
            Class<?> priorityClass = Class.forName("jdk.internal.crac.Core$Priority");
            Class<?> jdkResourceClass = Class.forName("jdk.internal.crac.JDKResource");
            Class<?> resourceClass = Class.forName("jdk.crac.Resource");
            Object[] priorities = priorityClass.getEnumConstants();
            if (priorities == null) {
                return;
            }
            Object normalPriority = null;
            for (Object priority : priorities) {
                if ("NORMAL".equals(priority.toString())) {
                    normalPriority = priority;
                }
            }
            if (normalPriority == null) {
                throw new IllegalStateException();
            }
            try {
                Method getContext = priorityClass.getMethod("getContext");
                Object ctx = getContext.invoke(normalPriority);
                Method register = ctx.getClass().getMethod("register", resourceClass);
                nativeInitResource = Proxy.newProxyInstance(null, new Class<?>[] { jdkResourceClass }, new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        if ("beforeCheckpoint".equals(method.getName())) {
                            // Do nothing
                        } else if ("afterRestore".equals(method.getName())) {
                            loadNativeLibrary();
                        } else if ("toString".equals(method.getName())) {
                            return toString();
                        } else if ("hashCode".equals(method.getName())) {
                            return hashCode();
                        } else if ("equals".equals(method.getName())) {
                            return equals(args[0]);
                        } else {
                            throw new UnsupportedOperationException(method.toString());
                        }
                        return null;
                    }
                });
                register.invoke(ctx, nativeInitResource);
            } catch (NoSuchMethodException | InvocationTargetException | IllegalAccessException e) {
                throw new IllegalStateException(e);
            }
        } catch (ClassNotFoundException e) {
            // ignored if class not present
        }
    }

    static native ByteBuffer getNativeMap(String imagePath);
}
