/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
import jdk.crac.management.*;
import jdk.internal.ref.CleanerFactory;
import jdk.internal.ref.CleanerImpl;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import static jdk.test.lib.Asserts.*;

import java.lang.reflect.Field;

/**
 * @test
 * @modules java.base/jdk.internal.ref
 * @library /test/lib
 * @build LinkedCleanableRefTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class LinkedCleanableRefTest implements CracTest {
    static private String RESTORED = "RESTORED";
    static private String CLEAN = "CLEAN=";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE).captureOutput(true);
        builder.vmOption("--add-opens=java.base/jdk.internal.ref=ALL-UNNAMED");
        builder.vmOption("--add-opens=java.base/java.lang.ref=ALL-UNNAMED");
        CracProcess process = builder.startCheckpoint();
        try {
            process.waitForSuccess();
            process.outputAnalyzer()
                    .shouldContain(RESTORED)
                    .shouldContain(CLEAN + "true");
        } finally {
            process.destroyForcibly();
        }
    }

    private static class Closer implements Runnable {
        private CleanerImpl.PhantomCleanableRef anotherRef;
        static private boolean cleanningInProgress = false;

        Closer(CleanerImpl.PhantomCleanableRef anotherRef) {
            this.anotherRef = anotherRef;
        }

        public void run() {
            if (null != anotherRef) {
                cleanningInProgress = true;
                anotherRef.clean();
                cleanningInProgress = false;
            } else {
                System.out.println(CLEAN + cleanningInProgress);
            }
        }
    }

    void setReferentToNull(CleanerImpl.PhantomCleanableRef ref) throws Exception {
        // Access to field: java.lang.ref.Reference.referent
        Field f = ref.getClass().getSuperclass().getSuperclass().getSuperclass().getDeclaredField("referent");
        f.setAccessible(true);
        f.set(ref, null);
    }

    @Override
    public void exec() throws Exception {
        var obj = new Object();
        var cleaner = CleanerFactory.cleaner();
        var ref = (CleanerImpl.PhantomCleanableRef) cleaner.register(obj, new Closer(null));
        var ref2 = (CleanerImpl.PhantomCleanableRef) cleaner.register(obj, new Closer(ref));

        setReferentToNull(ref);
        setReferentToNull(ref2);
        obj = null;

        Core.checkpointRestore();
        System.out.println("RESTORED");
    }
}
