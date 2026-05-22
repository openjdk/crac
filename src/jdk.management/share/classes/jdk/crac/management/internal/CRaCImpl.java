/*
 * Copyright (c) 2022, 2026, Azul Systems, Inc. All rights reserved.
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

package jdk.crac.management.internal;

import com.sun.management.internal.PlatformMBeanProviderImpl;
import jdk.crac.CheckpointException;
import jdk.crac.RestoreException;
import jdk.crac.management.CRaCMXBean;
import sun.management.Util;
import sun.management.VMManagement;

import javax.management.ObjectName;
import java.util.Arrays;

public class CRaCImpl implements CRaCMXBean {
    private final VMManagement vm;

    public CRaCImpl(VMManagement vm) {
        this.vm = vm;
    }

    @Override
    public long getUptimeSinceRestore() {
        return vm.getUptimeSinceRestore();
    }

    @Override
    public long getRestoreTime() {
        return vm.getRestoreTime();
    }

    @Override
    public CRaCMXBean.CheckpointableStatus getCheckpointableStatus() {
        int status = vm.getCheckpointableStatus();
        return CRaCMXBean.CheckpointableStatus.fromCode(status);
    }

    @Override
    public void checkpointRestore() throws RestoreException, CheckpointException {
        try {
            jdk.internal.crac.mirror.Core.checkpointRestore();
        } catch (jdk.internal.crac.mirror.CheckpointException e) {
            CheckpointException newException = new CheckpointException();
            Arrays.asList(e.getSuppressed()).forEach(newException::addSuppressed);
            throw newException;
        } catch (jdk.internal.crac.mirror.RestoreException e) {
            RestoreException newException = new RestoreException();
            Arrays.asList(e.getSuppressed()).forEach(newException::addSuppressed);
            throw newException;
        }
    }

    @Override
    public ObjectName getObjectName() {
        return Util.newObjectName(PlatformMBeanProviderImpl.CRAC_MXBEAN_NAME);
    }
}
