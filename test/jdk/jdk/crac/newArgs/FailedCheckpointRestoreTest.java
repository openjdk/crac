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

import java.util.List;

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;
import jdk.crac.RestoreException;
import static jdk.test.lib.Asserts.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.lib.process.OutputAnalyzer;

/**
 * @test
 * @summary New main should run only if C/R succeeds.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build FailedCheckpointRestoreTest
 * @run driver jdk.test.lib.crac.CracTest CHECKPOINT_EXCEPTION
 * @run driver jdk.test.lib.crac.CracTest RESTORE_EXCEPTION
 */
public class FailedCheckpointRestoreTest implements CracTest {
    private static final String NEW_MAIN_CLASS = "FailedCheckpointRestoreTest$InternalMain";
    private static final String RESTORE_OLD_MSG = "RESTORED IN OLD MAIN";
    private static final String RESTORE_NEW_MSG = "RESTORED IN NEW MAIN";

    public enum Variant {
        CHECKPOINT_EXCEPTION,
        RESTORE_EXCEPTION,
    };

    @CracTestArg(0)
    Variant variant;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().captureOutput(true);
        final OutputAnalyzer out;
        if (variant == Variant.CHECKPOINT_EXCEPTION) {
            out = builder.startCheckpoint().waitForSuccess().outputAnalyzer();
        } else {
            builder.doCheckpoint();
            out = builder.startRestoreWithArgs(null, List.of(NEW_MAIN_CLASS))
                .waitForSuccess().outputAnalyzer();
        }
        out.stdoutShouldNotContain(RESTORE_NEW_MSG).stdoutShouldNotContain(RESTORE_OLD_MSG);
    }

    @Override
    public void exec() {
        final var checkpointFail = "Failing on checkpoint!";
        final var restoreFail = "Failing on restore!";

        final var failingResource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
                if (variant == Variant.CHECKPOINT_EXCEPTION) {
                    throw new RuntimeException(checkpointFail);
                }
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) {
                if (variant == Variant.RESTORE_EXCEPTION) {
                    throw new RuntimeException(restoreFail);
                }
            }
        };
        Core.getGlobalContext().register(failingResource);

        try {
            Core.checkpointRestore();
            System.out.println(RESTORE_OLD_MSG);
        } catch (CheckpointException ex) {
            assertEquals(Variant.CHECKPOINT_EXCEPTION, variant, "Checkpoint failed unexpectedly", ex);
            assertEquals(1, ex.getSuppressed().length, "Number of suppressions", ex);
            assertEquals(checkpointFail, ex.getSuppressed()[0].getMessage(), "Suppression message", ex);
        } catch (RestoreException ex) {
            assertEquals(Variant.RESTORE_EXCEPTION, variant, "Restore failed unexpectedly", ex);
            assertEquals(1, ex.getSuppressed().length, "Number of suppressions", ex);
            assertEquals(restoreFail, ex.getSuppressed()[0].getMessage(), "Suppression message", ex);
        }
    }

    public static class InternalMain {
        public static void main(String[] args) {
            System.out.println(RESTORE_NEW_MSG);
        }
    }

    private static void assertEquals(Object lhs, Object rhs, String msg, Throwable cause) {
        if ((lhs != rhs) && ((lhs == null) || !(lhs.equals(rhs)))) {
            fail(msg + " expected: " + lhs + " but was: " + rhs, cause);
        }
    }
}
