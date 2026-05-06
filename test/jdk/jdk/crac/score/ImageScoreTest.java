/*
 * Copyright (c) 2025, 2026, Azul Systems, Inc. All rights reserved.
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
import jdk.crac.Resource;
import jdk.crac.management.CRaCMXBean;
import jdk.internal.crac.Score;
import jdk.test.lib.crac.*;

import java.io.File;
import java.lang.ref.Reference;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

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
    private static final String VM_UPTIME = "vm.uptime";
    private static final String JDK_CRAC_GLOBAL_CONTEXT_SIZE = "jdk.crac.globalContext.size";
    private static final String TEST_SCORE_AAA = "test.score.aaa";
    private static final String TEST_SCORE_BBB = "test.score.bbb";
    private static final String TEST_SCORE_CCC = "test.score.ccc";

    private static final double TEST_SCORE_AAA_VALUE_1 = 123.456;
    private static final double TEST_SCORE_BBB_VALUE_1 = 42.0;
    private static final double TEST_SCORE_CCC_VALUE_1 = 1.0;
    private static final double TEST_SCORE_AAA_VALUE_2 = 456.789;
    private static final double TEST_SCORE_CCC_VALUE_2 = 2.0;

    @CracTestArg
    boolean usePerfData;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE);
        builder.vmOption("-XX:" + (usePerfData ? '+' : '-') + "UsePerfData");
        builder.vmOption("--add-opens=java.base/jdk.internal.crac=ALL-UNNAMED");

        File pidFile = builder.imageDir().resolve("pid").toFile();
        if (pidFile.exists()) {
            assertTrue(pidFile.delete());
        }

        try (var process = builder.startCheckpoint()) {
            builder.clearVmOptions();
            process.waitForPausePid();

            final var score1 = parseRecordedScore(Files.readAllLines(builder.imageDir().resolve("score")));
            checkScore1(score1);

            builder.doRestore();
            process.clearPausePid();
            process.sendNewline();
            process.waitForPausePid();

            final var score2 = parseRecordedScore(Files.readAllLines(builder.imageDir().resolve("score")));
            checkScore2(score2);
            assertEquals(score1.get(JDK_CRAC_GLOBAL_CONTEXT_SIZE).intValue() + 1, score2.get(JDK_CRAC_GLOBAL_CONTEXT_SIZE).intValue());

            builder.doRestore();
            process.waitForSuccess();
        }
    }

    @Override
    public void exec() throws Exception {
        Score.setScore(TEST_SCORE_AAA, 0.001); // should be overwritten
        Score.setScore(TEST_SCORE_AAA, 123.456);
        Score.setScore(TEST_SCORE_BBB, 42);
        final double[] cccValue = new double[] {1};
        final Runnable cccProvider = () -> Score.setScore(TEST_SCORE_CCC, cccValue[0]);
        Score.addScoreProvider(cccProvider);
        // Force user-facing global context instantiation
        Context<Resource> globalContext = Context.getGlobalContext();
        checkScore1(Score.getScore());
        CRaCMXBean.getCRaCMXBean().checkpointRestore();

        assertEquals(System.in.read(), (int) '\n');

        Score.removeScore(TEST_SCORE_BBB);
        Score.setScore(TEST_SCORE_AAA, 456.789);
        cccValue[0] = 2; // Provider should set the updated value
        Resource dummy = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) {
            }
        };
        globalContext.register(dummy);
        checkScore2(Score.getScore());
        CRaCMXBean.getCRaCMXBean().checkpointRestore();

        Reference.reachabilityFence(cccProvider);
        Reference.reachabilityFence(dummy);
    }

    private static Map<String, Double> parseRecordedScore(List<String> lines) {
        final var score = new HashMap<String, Double>();
        for (var line : lines) {
            final var metric = line.substring(0, line.indexOf('='));
            final var value = Double.parseDouble(line.substring(line.indexOf('=') + 1));
            assertNull(score.put(metric, value), metric + " recorded multiple times");
        }
        return score;
    }

    private static void checkScore1(Map<String, Double> score) {
        assertGT(score.size(), 10, score.toString()); // at least 10 items
        assertTrue(score.containsKey(VM_UPTIME), score.toString());
        assertEquals(TEST_SCORE_AAA_VALUE_1, score.get(TEST_SCORE_AAA), score.toString());
        assertEquals(TEST_SCORE_BBB_VALUE_1, score.get(TEST_SCORE_BBB), score.toString());
        assertEquals(TEST_SCORE_CCC_VALUE_1, score.get(TEST_SCORE_CCC), score.toString());
    }

    private static void checkScore2(Map<String, Double> score) {
        assertGT(score.size(), 10, score.toString()); // at least 10 items
        assertTrue(score.containsKey(VM_UPTIME), score.toString());
        assertEquals(TEST_SCORE_AAA_VALUE_2, score.get(TEST_SCORE_AAA), score.toString());
        assertTrue(score.keySet().stream().noneMatch(m -> m.startsWith(TEST_SCORE_BBB)), score.toString());
        assertEquals(TEST_SCORE_CCC_VALUE_2, score.get(TEST_SCORE_CCC), score.toString());
    }
}
