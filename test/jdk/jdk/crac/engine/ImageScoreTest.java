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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;
import jdk.internal.crac.Score;
import jdk.test.lib.crac.*;

import java.io.File;
import java.lang.ref.Reference;
import java.nio.file.Files;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;

import static jdk.test.lib.Asserts.*;

/*
 * @test
 * @summary Check scores written by image_score extension
 * @requires (os.family == "linux")
 * @modules java.base/jdk.internal.crac:+open
 * @library /test/lib
 * @build ImageScoreTest
 * @run driver jdk.test.lib.crac.CracTest true
 * @run driver jdk.test.lib.crac.CracTest false
 */
public class ImageScoreTest implements CracTest {
    private static final String JDK_CRAC_GLOBAL_CONTEXT_SIZE = "jdk.crac.globalContext.size";
    private static final String TEST_SCORE_AAA = "test.score.aaa";
    private static final String TEST_SCORE_BBB = "test.score.bbb";
    public static final String RESTORE1 = "RESTORE1";
    public static final String RESTORE2 = "RESTORE2";

    @CracTestArg
    boolean usePerfData;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE).captureOutput(true);
        builder.vmOption("-XX:" + (usePerfData ? '+' : '-') + "UsePerfData");
        builder.vmOption("--add-opens=java.base/jdk.internal.crac=ALL-UNNAMED");

        File pidFile = builder.imageDir().resolve("pid").toFile();
        if (pidFile.exists()) {
            assertTrue(pidFile.delete());
        }
        final CracProcess process = builder.startCheckpoint();
        builder.clearVmOptions();
        process.waitForPausePid();

        List<String> score1 = Files.readAllLines(builder.imageDir().resolve("score"));
        assertGT(score1.size(), 10); // at least 10 items
        assertEquals(1L, score1.stream().filter(line -> line.startsWith("vm.uptime=")).count());
        int context1 = getScore(score1, JDK_CRAC_GLOBAL_CONTEXT_SIZE).intValue();
        // overwritten value should be there only once
        assertEquals(1L, score1.stream().filter(line-> line.startsWith(TEST_SCORE_AAA)).count());
        assertEquals(123.456, getScore(score1, TEST_SCORE_AAA));
        assertEquals(42.0   , getScore(score1, TEST_SCORE_BBB));
        builder.doRestore();
        CompletableFuture<?> f = new CompletableFuture<>();
        process.watch(line -> {
            if (line.equals(RESTORE1)) {
                try {
                    builder.doRestore();
                } catch (Exception e) {
                    f.completeExceptionally(e);
                }
            } else if (line.equals(RESTORE2)) {
                f.complete(null);
            }
        }, System.err::println);
        assertNull(f.get());

        List<String> score2 = Files.readAllLines(builder.imageDir().resolve("score"));
        int context2 = getScore(score2, JDK_CRAC_GLOBAL_CONTEXT_SIZE).intValue();
        assertEquals(context1 + 1, context2);
        assertEquals(456.789, getScore(score2, TEST_SCORE_AAA));
        assertTrue(score2.stream().noneMatch(line -> line.startsWith(TEST_SCORE_BBB)));
        process.waitForSuccess();
    }

    private static Double getScore(List<String> score1, String metric) {
        Optional<Double> contextSize = score1.stream().filter(line -> line.startsWith(metric + '='))
                .findFirst().map(line -> Double.parseDouble(line.substring(line.indexOf('=') + 1)));
        assertTrue(contextSize.isPresent());
        return contextSize.orElseThrow(() -> new AssertionError("context size not found"));
    }

    @Override
    public void exec() throws Exception {
        Score.setScore(TEST_SCORE_AAA, 0.001); // should be overwritten
        Score.setScore(TEST_SCORE_AAA, 123.456);
        Score.setScore(TEST_SCORE_BBB, 42);
        Core.checkpointRestore();

        Score.resetAll();
        Score.setScore(TEST_SCORE_AAA, 456.789);
        System.out.println(RESTORE1);
        Resource dummy = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) {
            }
        };
        Core.getGlobalContext().register(dummy);
        Core.checkpointRestore();

        Reference.reachabilityFence(dummy);
        System.out.println(RESTORE2);
    }
}
