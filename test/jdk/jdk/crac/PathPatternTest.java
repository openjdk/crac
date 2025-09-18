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

import jdk.crac.Core;
import jdk.test.lib.Platform;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.text.SimpleDateFormat;
import java.util.Comparator;
import java.util.Date;
import java.util.TimeZone;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @summary Checks that we can use patterns in CRaCCheckpointTo
 * @library /test/lib
 * @build PathPatternTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class PathPatternTest implements CracTest {
    private static final SimpleDateFormat DATE_FORMAT = new SimpleDateFormat("yyyyMMdd'T'HHmmss");
    private static final int MAX_DURATION_SECONDS = 300;

    static {
        DATE_FORMAT.setTimeZone(TimeZone.getTimeZone("UTC"));
    }

    private static long runCheckpoints(String pattern, boolean pause) throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE)
                .vmOption("-Xmx1G")
                .imageDir(pattern);
        if (pause) {
            builder.vmOption("-Dtest.pause=true");
        }
        return builder.startCheckpoint().waitForSuccess().pid();
    }

    @Override
    public void test() throws Exception {
        //noinspection ResultOfMethodCallIgnored
        new File("foo").mkdirs();

        // Fixed fields only, no-timestamps
        long pid = runCheckpoints("foo/cr_%%_%a_%p_%05c_%3m_%m_%g", false);
        File f = new File(String.format("foo/cr_%%_%s_%d_%05d_%d_1G_1",
                Platform.getOsArch(), pid, Runtime.getRuntime().availableProcessors(), 1024 * 1024 * 1024));
        assertTrue(f.exists(), "Expect" + f);
        assertTrue(f.isDirectory());
        deleteDir("foo");

        // Pattern-based checks
        runCheckpoints("foo/%u_%f_", false);
        try (var stream = Files.list(Path.of("foo"))) {
            AtomicInteger count = new AtomicInteger(0);
            assertTrue(stream.allMatch(d -> {
                count.incrementAndGet();
                if (!d.getFileName().toString().matches("\\p{XDigit}{8}-\\p{XDigit}{4}-\\p{XDigit}{4}-\\p{XDigit}{4}-\\p{XDigit}{12}_\\p{XDigit}{32}_")) {
                    return false;
                }
                return d.toFile().isDirectory();
            }));
            assertEquals(count.intValue(), 2);
        }
        deleteDir("foo");

        // Timestamps and generation
        runCheckpoints("foo/%T_%t_%015B_%b_%15R_%r_%02g", true);
        Pattern pattern = Pattern.compile("(\\d+)_(\\d{8}T\\d{6})Z_0*(\\d+)_(\\d{8}T\\d{6})Z_ *(\\d+)_(\\d{8}T\\d{6})Z_0(\\d)");
        try (var stream = Files.list(Path.of("foo"))) {
            Path[] paths = stream.toArray(Path[]::new);
            assertEquals(paths.length, 2);
            Matcher matcher1 = pattern.matcher(paths[0].getFileName().toString());
            assertTrue(matcher1.matches());
            Matcher matcher2 = pattern.matcher(paths[1].getFileName().toString());
            assertTrue(matcher2.matches());

            long t1 = Long.parseLong(matcher1.group(1));
            long t2 = Long.parseLong(matcher2.group(1));
            assertLT(t1, t2);
            long now = TimeUnit.MILLISECONDS.toSeconds(System.currentTimeMillis());
            assertLTE(t2, now);
            assertGT(t1 + MAX_DURATION_SECONDS, now);
            checkDateEquals(matcher1.group(2), t1);
            checkDateEquals(matcher2.group(2), t2);

            // boot time is constant
            long b1 = Long.parseLong(matcher1.group(3));
            long b2 = Long.parseLong(matcher1.group(3));
            assertEquals(b1, b2);
            assertLTE(b1, t1);
            assertGT(b1 + MAX_DURATION_SECONDS, now);
            checkDateEquals(matcher1.group(4), b1);
            checkDateEquals(matcher2.group(4), b2);

            long r1 = Long.parseLong(matcher1.group(5));
            long r2 = Long.parseLong(matcher2.group(5));
            assertEquals(r1, b1);
            assertLTE(t1, r2);
            assertLTE(r2, now);
            checkDateEquals(matcher1.group(6), b1);
            checkDateEquals(matcher2.group(6), b2);

            assertEquals("1", matcher1.group(7));
            assertEquals("2", matcher2.group(7));
        }
    }

    private static void checkDateEquals(String str, long ts) {
        assertEquals(str, DATE_FORMAT.format(new Date(TimeUnit.SECONDS.toMillis(ts))));
    }

    private static void deleteDir(String dir) throws IOException {
        Path dirPath = Path.of(dir);
        try (var stream = Files.walk(dirPath)) {
            stream.sorted(Comparator.reverseOrder()).forEach(p -> {
                if (!p.equals(dirPath)) {
                    try {
                        Files.delete(p);
                    } catch (IOException e) {
                        throw new RuntimeException(e);
                    }
                }
            });
        }
    }

    @Override
    public void exec() throws Exception {
        // Do two checkpoint-restores to ensure code behaves correctly after repeated execution
        Core.checkpointRestore();
        if (Boolean.getBoolean("test.pause")) {
            Thread.sleep(1000);
        }
        Core.checkpointRestore();
    }


}
