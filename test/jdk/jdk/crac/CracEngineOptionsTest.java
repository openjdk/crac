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
import org.junit.BeforeClass;
import static org.junit.Assert.*;

import jdk.test.lib.Platform;
import jdk.test.lib.Utils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.nio.file.Path;
import java.util.*;

/*
* @test
* @summary Testing CRaCEngine and CRaCEngineOptions VM options.
* @library /test/lib
* @build CracEngineOptionsTest
* @run junit/othervm CracEngineOptionsTest
*/
public class CracEngineOptionsTest {
    @BeforeClass
    public static void checkCriu() {
        final boolean hasCriu = Path.of(Utils.TEST_JDK, "lib", "criuengine").toFile().exists();
        assertEquals("CRIU exists iff we are on Linux", Platform.isLinux(), hasCriu);
    }

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
        if (Platform.isLinux()) {
            test("criu");
            test("criuengine");
        }

        final String absolute = Platform.isWindows() ?
            Path.of(Utils.TEST_JDK, "bin", "simengine.exe").toString() :
            Path.of(Utils.TEST_JDK, "lib", "simengine").toString();
        test(absolute);

        test("unknown", null, 1, "Cannot find CRaC engine unknown");
        test("simengine,--arg", null, 1, "Cannot find CRaC engine simengine,--arg");
        test("one two", null, 1, "Cannot find CRaC engine one two");
        test("", null, 1, "CRaCEngine must not be empty");
    }

    @Test
    public void test_options() throws Exception {
        test("simengine", "");
        test("simengine", "image_location=cr", 0,
                "Internal CRaC engine option provided, skipping: image_location");
        if (Platform.isLinux()) {
            test("criuengine", Arrays.asList("keep_running=true,args=-v -v -v -v"), 0,
                    Arrays.asList(
                        "CRaC engine option: 'keep_running' = 'true'",
                        "CRaC engine option: 'args' = '-v -v -v -v'"
                    ),
                    Arrays.asList("specified multiple times"));
            test("criuengine", "keep_running=true,args=-v -v -v -v,keep_running=false", 0,
                    "CRaC engine option: 'keep_running' = 'true'",
                    "CRaC engine option: 'args' = '-v -v -v -v'",
                    "CRaC engine option: 'keep_running' = 'false'",
                    "CRaC engine option 'keep_running' specified multiple times");
        }

        test("simengine", "help=true", 1,
                "unknown configure option: help",
                "CRaC engine failed to configure: 'help' = 'true'");
        test("simengine", "unknown=123", 1,
                "unknown configure option: unknown",
                "CRaC engine failed to configure: 'unknown' = '123'");
        test("simengine", "unknown=", 1,
                "unknown configure option: unknown",
                "CRaC engine failed to configure: 'unknown' = ''");
        test("simengine", "=", 1,
                "unknown configure option: \n",
                "CRaC engine failed to configure: '' = ''");
        test("simengine", "=,", 1,
                "unknown configure option: \n",
                "CRaC engine failed to configure: '' = ''");
        test("simengine", ",=", 1,
                "unknown configure option: \n",
                "CRaC engine failed to configure: '' = ''");
        test("simengine", ",", 1,
                "unknown configure option: \n",
                "CRaC engine failed to configure: '' = ''");

        if (Platform.isLinux()) {
            test("criuengine", "help,keep_running=true", 1,
                    "unknown configure option: help",
                    "CRaC engine failed to configure: 'help' = ''");
            test("criuengine", "direct_map=not a bool", 1,
                    "CRaC engine failed to configure: 'direct_map' = 'not a bool'");
        }
    }

    @Test
    public void test_options_separated() throws Exception {
        test("simengine",
                Arrays.asList(
                    "args=simengine ignores this",
                    "args=another arg,keep_running=true,args=and another",
                    "args=this is also ignored"
                ),
                0,
                Arrays.asList(
                    "CRaC engine option: 'args' = 'simengine ignores this'",
                    "CRaC engine option: 'args' = 'another arg'",
                    "CRaC engine option: 'keep_running' = 'true'",
                    "CRaC engine option: 'args' = 'and another'",
                    "CRaC engine option: 'args' = 'this is also ignored'",
                    "CRaC engine option 'args' specified multiple times"
                ),
                Collections.emptyList());

        test("simengine",
                Arrays.asList("args=--arg1 --arg2", "--arg3"),
                1,
                Arrays.asList(
                    "CRaC engine option: 'args' = '--arg1 --arg2'",
                    "unknown configure option: --arg3",
                    "CRaC engine failed to configure: '--arg3' = ''"
                ),
                Arrays.asList("specified multiple times"));

        if (Platform.isLinux()) {
            test("criuengine",
                    Arrays.asList("help", "args=-v4"),
                    1,
                    Arrays.asList(
                        "unknown configure option: help",
                        "CRaC engine failed to configure: 'help' = ''"
                    ),
                    Collections.emptyList());
        }
    }

    @Test
    public void test_options_help() throws Exception {
        testHelp();
        testHelp("-XX:CRaCCheckpointTo=cr");
        testHelp("-XX:CRaCRestoreFrom=cr");
    }

    private void test(String engine) throws Exception {
        test(engine, Collections.emptyList(), 0, Collections.emptyList(), Collections.emptyList());
    }

    private void test(String engine, String opts) throws Exception {
        test(engine, opts != null ? Arrays.asList(opts) : Collections.emptyList(),
            0, Collections.emptyList(), Collections.emptyList());
    }

    private void test(String engine, String opts, int expectedExitValue,
            String... expectedTexts) throws Exception {
        test(engine, opts != null ? Arrays.asList(opts) : Collections.emptyList(),
                expectedExitValue, Arrays.asList(expectedTexts), Collections.emptyList());
    }

    private void test(String engine, List<String> opts, int expectedExitValue,
            List<String> expectedTexts, List<String> notExpectedTexts) throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                "-XX:CRaCCheckpointTo=cr",
                "-XX:CRaCEngine=" + engine,
                "-Xlog:crac=debug",
                "-version");
        for (String opt : opts) {
            pb.command().add(pb.command().size() - 2, "-XX:CRaCEngineOptions=" + opt);
        }
        OutputAnalyzer out = new OutputAnalyzer(pb.start());
        out.shouldHaveExitValue(expectedExitValue);
        for (String text : expectedTexts) {
            out.shouldContain(text);
        }
        for (String text : notExpectedTexts) {
            out.shouldNotContain(text);
        }
    }

    private static void testHelp(String... opts) throws Exception {
        List<String> optsList = new ArrayList(Arrays.asList(opts));
        optsList.add("-XX:CRaCEngineOptions=help");
        optsList.add("-Xlog:crac=debug");
        // Limited to not get non-restore-settable flags with CRaCRestoreFrom
        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(optsList);
        OutputAnalyzer out = new OutputAnalyzer(pb.start());
        out.shouldHaveExitValue(0);
        out.stdoutShouldContain("Configuration options:");
        out.stderrShouldBeEmpty();
        out.shouldNotContain("CRaC engine option:");
    }
}
