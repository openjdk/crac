/*
 * Copyright (c) 2023-2025, Azul Systems, Inc. All rights reserved.
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

import org.junit.Test;

import jdk.test.lib.Utils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.nio.file.Path;

/*
* @test
* @summary Testing CRaCEngine and CRaCEngineOptions VM options.
* @library /test/lib
* @build CracEngineOptionsTest
* @run junit/othervm CracEngineOptionsTest
*/
public class CracEngineOptionsTest {
    @Test
    public void test_default() throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:CRaCCheckpointTo=cr",
                "-version");
        OutputAnalyzer out = new OutputAnalyzer(pb.start());
        out.shouldHaveExitValue(0);
    }

    @Test
    public void test_engines() throws Exception {
        test("sim");
        test("simengine");
        test("pause");
        test("pauseengine");
        if ("linux".equals(System.getProperty("os.family"))) {
            test("criu");
            test("criuengine");
        }

        final String absolute;
        if ("windows".equals(System.getProperty("os.family"))) {
            absolute = Path.of(Utils.TEST_JDK, "bin", "simengine.exe").toString();
        } else {
            absolute = Path.of(Utils.TEST_JDK, "lib", "simengine").toString();
        }
        test(absolute);

        test("unknown", null, 1, "Cannot find CRaC engine unknown");
        test("simengine,--arg", null, 1, "Cannot find CRaC engine simengine,--arg");
        test("one two", null, 1, "Cannot find CRaC engine one two");
        test("", null, 1, "CRaCEngine must not be empty");
    }

    @Test
    public void test_options() throws Exception {
        test("simengine", "");
        test("simengine", "help", 0,
                "CRaC engine option: 'help' = ''",
                "Configuration options:"); // A line from the help message
        test("simengine", "help=true", 0,
                "CRaC engine option: 'help' = 'true'",
                "Configuration options:");
        test("simengine", "image_location=cr", 0,
                "Internal CRaC engine option provided, skipping: image_location");
        if ("linux".equals(System.getProperty("os.family"))) {
            test("criuengine", "keep_running=true,args=-v -v -v -v,keep_running=false", 0,
                    "CRaC engine option: 'keep_running' = 'true'",
                    "CRaC engine option: 'args' = '-v -v -v -v'",
                    "CRaC engine option: 'keep_running' = 'false'");
        }

        test("simengine", "unknown=123", 1,
                "CRaC engine does not support provided option: unknown");
        test("simengine", "help,unknown=123", 1,
                "CRaC engine does not support provided option: unknown");
        test("simengine", "unknown=", 1,
                "CRaC engine does not support provided option: unknown");
        test("simengine", "=", 1,
                "CRaC engine does not support provided option: \n"); // \n to check there's nothing more
        test("simengine", "=,", 1,
                "CRaC engine does not support provided option: \n");
        test("simengine", ",=", 1,
                "CRaC engine does not support provided option: \n");
        test("simengine", ",", 1,
                "CRaC engine does not support provided option: \n");

        if ("linux".equals(System.getProperty("os.family"))) {
            test("criuengine", "direct_map=not a bool", 1,
                    "CRaC engine failed to configure: 'direct_map' = 'not a bool'");
        }
    }

    private void test(String engine) throws Exception {
        test(engine, null, 0);
    }

    private void test(String engine, String opts) throws Exception {
        test(engine, opts, 0);
    }

    private void test(String engine, String opts, int expectedExitValue, String... expectedTexts) throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:CRaCCheckpointTo=cr",
                "-XX:CRaCEngine=" + engine,
                "-Xlog:crac=debug",
                "-version");
        if (opts != null) {
            pb.command().add(pb.command().size() - 2, "-XX:CRaCEngineOptions=" + opts);
        }
        OutputAnalyzer out = new OutputAnalyzer(pb.start());
        out.shouldHaveExitValue(expectedExitValue);
        for (String text : expectedTexts) {
            out.shouldContain(text);
        }
    }
}
