/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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

import jdk.internal.crac.Score;

import java.util.Map;

import static jdk.test.lib.Asserts.*;

/*
 * @test
 * @summary Tests scores computation in presence of failing score providers.
 * @modules java.base/jdk.internal.crac:+open
 * @library /test/lib
 */
public class FailingScoreProviderTest {
    private static final String VM_UPTIME = "vm.uptime";
    private static final String JDK_CRAC_INTERNAL_CONTEXT_SIZE = "jdk.crac.internalContext.size";
    private static final String TEST_SCORE = "test.score";
    private static final double TEST_SCORE_VALUE = 123.456;

    // Strong references
    private static final Runnable NORMAL_PROVIDER = () -> Score.setScore(TEST_SCORE, TEST_SCORE_VALUE);
    private static final Runnable FAILING_PROVIDER = () -> {
        throw new RuntimeException("");
    };
    private static final Runnable CRASHING_PROVIDER = () -> {
        throw new ExpectedError();
    };

    public static void main(String[] args) {
        // Provider exceptions should be ignored
        Score.addScoreProvider(FAILING_PROVIDER);
        Score.addScoreProvider(NORMAL_PROVIDER);
        checkScores(Score.getScores());

        // Provider errors should be propagated
        Score.addScoreProvider(CRASHING_PROVIDER);
        assertThrows(ExpectedError.class, Score::getScores);
    }

    private static void checkScores(Map<String, Double> scores) {
        assertGT(scores.size(), 10, scores.toString()); // at least 10 items
        assertTrue(scores.containsKey(VM_UPTIME), scores.toString());
        assertTrue(scores.containsKey(JDK_CRAC_INTERNAL_CONTEXT_SIZE), scores.toString());
        assertEquals(TEST_SCORE_VALUE, scores.get(TEST_SCORE), scores.toString());
    }

    private static class ExpectedError extends Error {
    }
}
