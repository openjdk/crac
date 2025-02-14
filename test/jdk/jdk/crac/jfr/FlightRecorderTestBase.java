/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

import jdk.jfr.consumer.RecordingFile;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;

import static jdk.test.lib.Asserts.assertTrue;
import static jdk.test.lib.Asserts.fail;

abstract class FlightRecorderTestBase implements CracTest {
    protected static void assertRecording(File jfr) {
        assertTrue(jfr.exists());
        assertTrue(jfr.length() > 0);
        try (RecordingFile recordingFile = new RecordingFile(jfr.toPath())) {
            while (recordingFile.hasMoreEvents()) {
                recordingFile.readEvent();
            }
        } catch (IOException e) {
            fail("Cannot read JFR file", e);
        }
        assertTrue(jfr.delete());
    }
}
