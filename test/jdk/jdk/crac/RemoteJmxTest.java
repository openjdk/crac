/*
 * Copyright (c) 2022, 2026 Azul Systems, Inc. All rights reserved.
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

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;
import jdk.crac.RestoreException;
import jdk.crac.management.*;

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.EOFException;
import java.io.IOException;
import java.lang.ref.Reference;
import java.lang.reflect.UndeclaredThrowableException;
import java.rmi.UnmarshalException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;
import javax.management.JMX;
import javax.management.MalformedObjectNameException;
import javax.management.ObjectName;
import javax.management.remote.JMXConnector;
import javax.management.remote.JMXConnectorFactory;
import javax.management.remote.JMXServiceURL;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @build RemoteJmxTest
 * @run driver jdk.test.lib.crac.CracTest 9995 none
 * @run driver jdk.test.lib.crac.CracTest 9995 10095
 * @run driver jdk.test.lib.crac.CracTest none 10095
 */
public class RemoteJmxTest implements CracTest {
    private static final String BOOTED = "BOOTED";
    private static final String RESTORED = "RESTORED";

    public static final String NONE = "none";

    @CracTestArg(0)
    String portBefore;

    @CracTestArg(1)
    String portAfter;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        if (!NONE.equals(portBefore)) {
            javaOptions(portBefore).forEach(builder::javaOption);
        }

        try (var process = builder.startCheckpoint();) {
            process.waitForStdout(BOOTED, true);
            if (!NONE.equals(portBefore)) {
                try {
                    withCRaCMXBean(portBefore, bean -> {
                        assertEquals(-1L, bean.getUptimeSinceRestore());
                        try {
                            bean.checkpointRestore();
                        } catch (CheckpointException | RestoreException e) {
                            throw new RuntimeException(e);
                        }
                        return null;
                    });
                } catch (UndeclaredThrowableException e) {
                    assertEquals(UnmarshalException.class, e.getCause().getClass());
                    assertEquals(EOFException.class, e.getCause().getCause().getClass());
                }
            }
            process.sendNewline();

            process.waitForStdout(RESTORED, false /* skip checkpoint log */);
            String currentPort = NONE.equals(portAfter) ? portBefore : portAfter;
            assertGreaterThanOrEqual(withCRaCMXBean(currentPort, CRaCMXBean::getUptimeSinceRestore), 0L);
            process.sendNewline();

            process.waitForSuccess();
        }
    }

    private static Map<String, String> javaOptions(String port) {
        // the options are in a map, so the values will be overwritten
        var opts = new HashMap<String, String>();
        opts.put("java.rmi.server.hostname", "localhost");
        opts.put("com.sun.management.jmxremote", "true");
        opts.put("com.sun.management.jmxremote.port", port);
        opts.put("com.sun.management.jmxremote.rmi.port", port);
        opts.put("com.sun.management.jmxremote.ssl", "false");
        opts.put("com.sun.management.jmxremote.authenticate", "false");
        opts.put("sun.rmi.transport.tcp.logLevel", "FINER");
        opts.put("jdk.crac.collect-fd-stacktraces", "true");
        return opts;
    }

    private static <T> T withCRaCMXBean(String port, Function<CRaCMXBean, T> consumer) throws IOException, MalformedObjectNameException {
        try (JMXConnector conn = JMXConnectorFactory.connect(new JMXServiceURL("service:jmx:rmi:///jndi/rmi://localhost:" + port + "/jmxrmi"))) {
            return consumer.apply(JMX.newMBeanProxy(conn.getMBeanServerConnection(), new ObjectName("jdk.management:type=CRaC"), CRaCMXBean.class));
        }
    }

    @Override
    public void exec() throws Exception {
        CountDownLatch latch = new CountDownLatch(1);
        Resource resource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) {
                latch.countDown();
            }
        };
        Core.getGlobalContext().register(resource);
        System.out.println(BOOTED);
        if (!NONE.equals(portAfter)) {
            javaOptions(portAfter).forEach(System::setProperty);
        }
        assertEquals((int)'\n', System.in.read());
        if (NONE.equals(portBefore)) {
            Core.checkpointRestore();
        } else {
            /* checkpoint remotely triggered here - let's just wait for that */
            assertTrue(latch.await(30, TimeUnit.SECONDS));
            /* Busy wait until we can connect to ourselves */
            while (true) {
                String currentPort = NONE.equals(portAfter) ? portBefore : portAfter;
                try {
                    withCRaCMXBean(currentPort, CRaCMXBean::getRestoreTime);
                    break;
                } catch (IOException e) {
                    Thread.sleep(100);
                }
            }
        }
        /* RESTORED must not be printed until the restore fully completes; JMX server is started only after all resources */
        System.out.println(RESTORED);
        assertEquals((int)'\n', System.in.read());
        Reference.reachabilityFence(resource);
    }
}
