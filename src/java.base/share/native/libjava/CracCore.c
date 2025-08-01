/*
 * Copyright (c) 2017, 2025, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

/** \file */

#include "jni.h"
#include "jvm.h"
#include "jni_util.h"
#include "io_util.h"
#include "io_util_md.h"

#include "jdk_internal_crac_mirror_Core.h"

JNIEXPORT jobjectArray JNICALL
Java_jdk_internal_crac_mirror_Core_checkpointRestore0(JNIEnv *env, jclass ignore, jarray fdArr, jobjectArray objArr, jboolean dry_run, jlong jcmd_stream) {
    return JVM_Checkpoint(env, fdArr, objArr, dry_run, jcmd_stream);
}

JNIEXPORT void JNICALL
Java_jdk_internal_crac_mirror_Core_startRecordingDecompilations0(JNIEnv *env, jclass ignore) {
    JVM_StartRecordingDecompilations(env);
}

JNIEXPORT void JNICALL
Java_jdk_internal_crac_mirror_Core_finishRecordingDecompilationsAndRecompile0(JNIEnv *env, jclass ignore) {
    JVM_FinishRecordingDecompilationsAndRecompile(env);
}
