/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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

package jdk.internal.util.jar;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.Core;
import jdk.internal.crac.LoggerContainer;
import jdk.internal.crac.JDKResource;

import java.io.File;
import java.io.IOException;
import java.util.jar.JarFile;

/**
 * @crac It is assumed that JAR files opened through this class that are open
 * during checkpoint will be present on same path in the filesystem after
 * restore. Therefore, application does <strong>not</strong> have to close
 * these files before a checkpoint.
 */
public class PersistentJarFile extends JarFile implements JDKResource {

    public PersistentJarFile(File file, boolean b, int openRead, Runtime.Version runtimeVersion) throws IOException {
        super(file, b, openRead, runtimeVersion);
        Core.Priority.NORMAL.getContext().register(this);
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        LoggerContainer.info(this.getName() + " is recorded as always available on restore");
        SharedSecrets.getJavaUtilZipFileAccess().beforeCheckpoint(this);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        // do nothing, no fixup required
    }
}
