/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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

#include <jvmti.h>

#include <string.h>

void JNICALL callbackBeforeCheckpoint(jvmtiEnv* jvmti_env, ...) {
    printf("%s:%d : %s\n", __FILE__, __LINE__, __FUNCTION__);
    fflush(NULL);
}

void JNICALL callbackAfterRestore(jvmtiEnv* jvmti_env, ...) {
    printf("%s:%d : %s\n", __FILE__, __LINE__, __FUNCTION__);
    fflush(NULL);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* reserved) {
    printf("%s:%d : %s : JVMTI agent loading...\n", __FILE__, __LINE__, __FUNCTION__);

    jvmtiEnv* jvmti = NULL;
    jint extensionEventCount = 0;
    jvmtiExtensionEventInfo* extensionEvents = NULL;
    (*jvm)->GetEnv(jvm, (void**)&jvmti, JVMTI_VERSION_1_0);
    (*jvmti)->GetExtensionEvents(jvmti, &extensionEventCount, &extensionEvents);

    for (int i = 0; i < extensionEventCount; ++i) {
        if (0 == strcmp("jdk.crac.events.BeforeCheckpoint", extensionEvents[i].id)) {
            (*jvmti)->SetExtensionEventCallback(jvmti, extensionEvents[i].extension_event_index, &callbackBeforeCheckpoint);
        }
        if (0 == strcmp("jdk.crac.events.AfterRestore", extensionEvents[i].id)) {
            (*jvmti)->SetExtensionEventCallback(jvmti, extensionEvents[i].extension_event_index, &callbackAfterRestore);
        }
    }

    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* jvm) {
    printf("%s:%d : %s : JVMTI agent unloading...\n", __FILE__, __LINE__, __FUNCTION__);
}
