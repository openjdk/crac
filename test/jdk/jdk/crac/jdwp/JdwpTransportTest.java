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
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import com.sun.jdi.Bootstrap;
import com.sun.jdi.connect.AttachingConnector;
import com.sun.jdi.connect.Connector;
import com.sun.jdi.connect.IllegalConnectorArgumentsException;
import com.sun.jdi.VirtualMachine;
import com.sun.jdi.VMDisconnectedException;

import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.io.IOException;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import static jdk.test.lib.Asserts.*;

/**
 * @test ContextOrderTest
 * @library /test/lib
 * @build JdwpTransportTest
 * @run driver/timeout=30 jdk.test.lib.crac.CracTest false false
 * @run driver/timeout=30 jdk.test.lib.crac.CracTest true false
 * @run driver/timeout=30 jdk.test.lib.crac.CracTest true true
 */

public class JdwpTransportTest implements CracTest {
    @CracTestArg(0)
    boolean suspendOnJdwpStart;

    @CracTestArg(1)
    boolean keepDebuggingBeforeCheckpoint;

    private static String ATTACH_CONNECTOR = "com.sun.jdi.SocketAttach";
    private static String ADDRESS = "127.0.0.1";
    private static String PORT = "5555";
    private static String DEBUGEE = "Listening for transport dt_socket at address: 5555";
    private static String STARTED = "APP: Started";
    private static String CHECKPOINT = "CR: Checkpoint";

    private VirtualMachine attachDebugger() throws Exception {
        AttachingConnector ac = Bootstrap.virtualMachineManager().attachingConnectors()
                .stream().filter(c -> c.name().equals(ATTACH_CONNECTOR)).findFirst()
                .orElseThrow(() -> new RuntimeException("Unable to locate " + ATTACH_CONNECTOR));

        Map<String, Connector.Argument> args = ac.defaultArguments();

        Connector.Argument argHost = args.get("hostname");
        argHost.setValue(ADDRESS);
        Connector.Argument argPort = args.get("port");
        argPort.setValue(PORT);
        System.out.println("TEST: Debugger is attaching to: " + ADDRESS + ":" + PORT + " ...");

        VirtualMachine vm = ac.attach(args);
        System.out.println("TEST: Attached!");
        System.out.println("TEST: Get all threads");
        vm.allThreads().stream().forEach(System.out::println);
        return vm;
    }

    private void waitForString(BufferedReader reader, String str) throws IOException {
        for (String line = reader.readLine(); true; line = reader.readLine()) {
            System.out.println(line);
            if (line.contains(str)) 
                break;
        }
    }

    @Override
    public void test() throws Exception {
        final String suspendArg = ",suspend=" + (suspendOnJdwpStart ? "y" : "n");
        CracBuilder builder = new CracBuilder()
            .vmOption("-agentlib:jdwp=transport=dt_socket,server=y,address=0.0.0.0:" + PORT + suspendArg);
        builder.engine(CracEngine.SIMULATE);

        CracProcess process = builder.captureOutput(true).startCheckpoint();
        try {
            var reader = new BufferedReader(new InputStreamReader(process.output()));
            if (!suspendOnJdwpStart) {
                waitForString(reader, STARTED);
            } else {
                waitForString(reader, DEBUGEE);
                VirtualMachine vm = attachDebugger();
                if (keepDebuggingBeforeCheckpoint) {
                    vm.resume();
                    System.out.println("TEST: Debugger resume.");
                    try {
                        waitForString(reader, CHECKPOINT);
                        vm.dispose();
                        fail("VMDisconnectedException isn't thrown. The debugger should be disconnected by a debuggee.");
                    } catch (VMDisconnectedException e) {
                    }
                } else {
                    vm.dispose();
                    vm = null;
                    System.out.println("TEST: Debugger done.");

                    waitForString(reader, STARTED);
                }
            }

            // After checkpoint/restore
            waitForString(reader, DEBUGEE);
            VirtualMachine vm = attachDebugger();
            vm.dispose();
            System.out.println("TEST: Debugger done.");
            System.out.flush();
            process.input().write(0); // Resume app
            process.input().flush();

            process.waitForSuccess();
        } finally {
            process.destroyForcibly();
        }
    }

    @Override
    public void exec() throws Exception {
        System.out.println(STARTED);
        Core.checkpointRestore();
        System.out.println("APP: Restored");
        System.in.read(); // Wait for debugger is attached and done
        System.out.println("APP: Finished");
    }
}

