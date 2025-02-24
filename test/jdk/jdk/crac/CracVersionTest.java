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

 import org.junit.Test;

 import jdk.test.lib.process.OutputAnalyzer;
 import jdk.test.lib.process.ProcessTools;
 /*
  * @test CracVersionTest
  * @library /test/lib
  * @build CracVersionTest
  * @run junit/othervm CracVersionTest
  */
 public class CracVersionTest {
     @Test
     public void test_default() throws Exception {
         ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                 "-XX:CRaCCheckpointTo=cr",
                 "-version");
         OutputAnalyzer out = new OutputAnalyzer(pb.start());
         out.shouldHaveExitValue(0);
     }

     private final String UNKNOWN_ENGINE = "unknown";

     @Test
     public void test_fail() throws Exception {
         ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(
                 "-XX:CRaCCheckpointTo=cr",
                 "-XX:CRaCEngine=" + UNKNOWN_ENGINE,
                 "-version");
         OutputAnalyzer out = new OutputAnalyzer(pb.start());
         out.shouldHaveExitValue(1);
         if (System.getProperty("os.name").contains("Windows")) {
            out.shouldContain(UNKNOWN_ENGINE + ".exe:");
         } else {
            out.shouldContain(UNKNOWN_ENGINE + ":");
         }
     }
 }
