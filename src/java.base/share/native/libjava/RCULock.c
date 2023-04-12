/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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

#include <stdlib.h>
#include <string.h>

#include "jni.h"
#include "jvm.h"
#include "jni_util.h"
#include "jdk_crac_RCULock.h"

static jfieldID readerThreadsListField = NULL;
static jfieldID readCriticalMethodsField = NULL;

JNIEXPORT void JNICALL
Java_jdk_crac_RCULock_initFieldOffsets(JNIEnv *env, jclass cls)
{
    readerThreadsListField = (*env)->GetFieldID(env, cls, "readerThreadsList", "J");
    readCriticalMethodsField = (*env)->GetFieldID(env, cls, "readCriticalMethods", "J");
}

static void free_up_to(char **mem, uint limit) {
    for (uint i = 0; i < limit; ++i) {
        free(*(mem + i));
    }
    free(mem);
}

JNIEXPORT void JNICALL
Java_jdk_crac_RCULock_init(JNIEnv *env, jobject rcuLock, jobjectArray methods)
{
    void *threads = JVM_ThreadListAllocate();
    if (threads == NULL) {
        JNU_ThrowOutOfMemoryError(env, NULL);
        return;
    }
    (*env)->SetLongField(env, rcuLock, readerThreadsListField, (jlong) threads);

    jsize num = (*env)->GetArrayLength(env, methods);
    char **c_methods = malloc(sizeof(char *) * (num + 1));
    if (c_methods == NULL) {
        JNU_ThrowOutOfMemoryError(env, NULL);
        return;
    }
    for (int i = 0; i < num; ++i) {
        jobject el = (*env)->GetObjectArrayElement(env, methods, i);
        if (el == NULL) {
            free_up_to(c_methods, i);
            JNU_ThrowNullPointerException(env, "null signature");
            return;
        }
        jsize len = (*env)->GetStringLength(env, el);
        const char *sig = (*env)->GetStringUTFChars(env, el, NULL);
        if (sig == NULL) {
            free_up_to(c_methods, i);
            JNU_ThrowNullPointerException(env, "null signature");
            return;
        }
        char *copy = malloc((len + 1) * sizeof(char));
        if (copy == NULL) {
            (*env)->ReleaseStringUTFChars(env, el, sig);
            free_up_to(c_methods, i);
            JNU_ThrowOutOfMemoryError(env, NULL);
            return;
        }
        strncpy(copy, sig, len + 1);
        (*env)->ReleaseStringUTFChars(env, el, sig);
        c_methods[i] = copy;
    }
    c_methods[num] = NULL;
#ifndef PROD
    // TODO: assert methods are sorted - note that we should mind UTF-8 format
#endif // !PROD
    (*env)->SetLongField(env, rcuLock, readCriticalMethodsField, (jlong) (void *) c_methods);
}

JNIEXPORT void JNICALL
Java_jdk_crac_RCULock_destroy(JNIEnv *env, jobject rcuLock)
{
    jlong threads = (*env)->GetLongField(env, rcuLock, readerThreadsListField);
    if (threads != 0) {
        JVM_ThreadListDestroy((void *) threads);
        (*env)->SetLongField(env, rcuLock, readerThreadsListField, 0);
    }

    jlong methods = (*env)->GetLongField(env, rcuLock, readCriticalMethodsField);
    if (methods != 0) {
        char **m = (char **) (void *) methods;
        while (*m != NULL) {
            free(*m);
            ++m;
        }
        free(m);
        (*env)->SetLongField(env, rcuLock, readCriticalMethodsField, 0);
    }
}

JNIEXPORT void JNICALL
Java_jdk_crac_RCULock_removeThread(JNIEnv *env, jobject rcuLock)
{
    jlong addr = (*env)->GetLongField(env, rcuLock, readerThreadsListField);
    if (addr != 0) {
        JVM_ThreadListRemoveSelf((void *) addr);
    }
}

JNIEXPORT jboolean JNICALL
Java_jdk_crac_RCULock_hasReaderThreads(JNIEnv *env, jobject rcuLock)
{
    jlong addr = (*env)->GetLongField(env, rcuLock, readerThreadsListField);
    if (addr != 0) {
        return JVM_ThreadListLength((void *) addr) != 0;
    } else {
        return 0;
    }
}

JNIEXPORT void JNICALL
Java_jdk_crac_RCULock_synchronizeThreads(JNIEnv *env, jobject rcuLock)
{
    jlong threads = (*env)->GetLongField(env, rcuLock, readerThreadsListField);
    jlong methods = (*env)->GetLongField(env, rcuLock, readCriticalMethodsField);
    JVM_RCU_SynchronizeThreads((void *) threads, (const char **) (void *) methods);
}
