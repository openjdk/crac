/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.*;
import jdk.crac.management.*;

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
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
 * @run driver jdk.test.lib.crac.CracTest
 */
public class RemoteJmxTest implements CracTest {
    private static final String PORT = "9995";
    private static final String BOOTED = "BOOTED";
    private static final String RESTORED = "RESTORED";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().captureOutput(true);
        builder.engine(CracEngine.SIMULATE);
        builder.javaOption("java.rmi.server.hostname", "localhost")
                .javaOption("com.sun.management.jmxremote", "true")
                .javaOption("com.sun.management.jmxremote.port", PORT)
                .javaOption("com.sun.management.jmxremote.rmi.port", PORT)
                .javaOption("com.sun.management.jmxremote.ssl", "false")
                .javaOption("com.sun.management.jmxremote.authenticate", "false")
                .javaOption("sun.rmi.transport.tcp.logLevel", "FINER")
                .javaOption("jdk.crac.collect-fd-stacktraces", "true");
        CracProcess process = builder.startCheckpoint();
        try {
            var reader = new BufferedReader(new InputStreamReader(process.output()));
            waitForString(reader, BOOTED);
            assertEquals(-1L, getUptimeFromRestoreFromJmx());
            process.sendNewline();
            waitForString(reader, RESTORED);
            assertGreaterThanOrEqual(getUptimeFromRestoreFromJmx(), 0L);
            process.sendNewline();
            process.waitForSuccess();
        } finally {
            process.destroyForcibly();
        }
    }

    private void waitForString(BufferedReader reader, String str) throws IOException {
        for (String line = reader.readLine(); true; line = reader.readLine()) {
            System.out.println(line);
            if (line.contains(str))
                break;
        }
    }

    private long getUptimeFromRestoreFromJmx() throws IOException, MalformedObjectNameException {
        JMXConnector conn = JMXConnectorFactory.connect(new JMXServiceURL("service:jmx:rmi:///jndi/rmi://localhost:" + PORT + "/jmxrmi"));
        CRaCMXBean cracBean = JMX.newMBeanProxy(conn.getMBeanServerConnection(), new ObjectName("jdk.management:type=CRaC"), CRaCMXBean.class);
        return cracBean.getUptimeSinceRestore();
    }

    @Override
    public void exec() throws Exception {
        System.out.println(BOOTED);
        assertEquals((int)'\n', System.in.read());
        Core.checkpointRestore();
        System.out.println(RESTORED);
        assertEquals((int)'\n', System.in.read());
    }
}
