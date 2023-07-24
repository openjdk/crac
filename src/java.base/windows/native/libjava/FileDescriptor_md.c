/*
 * Copyright (c) 2003, 2018, Oracle and/or its affiliates. All rights reserved.
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

#include "jni.h"
#include "jni_util.h"
#include "jvm.h"
#include "io_util.h"
#include "jlong.h"
#include "io_util_md.h"

#include <windows.h>
#include <winternl.h>

#include "java_io_FileDescriptor.h"

/*******************************************************************/
/*  BEGIN JNI ********* BEGIN JNI *********** BEGIN JNI ************/
/*******************************************************************/

/* field id for jint 'fd' in java.io.FileDescriptor */
jfieldID IO_fd_fdID;

/* field id for jlong 'handle' in java.io.FileDescriptor */
jfieldID IO_handle_fdID;

/* field id for jboolean 'append' in java.io.FileDescriptor */
jfieldID IO_append_fdID;

/**************************************************************
 * static methods to store field IDs in initializers
 */

JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_initIDs(JNIEnv *env, jclass fdClass) {
    CHECK_NULL(IO_fd_fdID = (*env)->GetFieldID(env, fdClass, "fd", "I"));
    CHECK_NULL(IO_handle_fdID = (*env)->GetFieldID(env, fdClass, "handle", "J"));
    CHECK_NULL(IO_append_fdID = (*env)->GetFieldID(env, fdClass, "append", "Z"));
}

/**************************************************************
 * File Descriptor
 */

JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_sync(JNIEnv *env, jobject this) {
    FD fd = THIS_FD(this);
    if (IO_Sync(fd) == -1) {
        JNU_ThrowByName(env, "java/io/SyncFailedException", "sync failed");
    }
}

JNIEXPORT jlong JNICALL
Java_java_io_FileDescriptor_getHandle(JNIEnv *env, jclass fdClass, jint fd) {
    SET_HANDLE(fd);
}

JNIEXPORT jboolean JNICALL
Java_java_io_FileDescriptor_getAppend(JNIEnv *env, jclass fdClass, jint fd) {
    return JNI_FALSE;
}

// instance method close0 for FileDescriptor
JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_close0(JNIEnv *env, jobject this) {
    fileDescriptorClose(env, this);
}

JNIEXPORT void JNICALL
Java_java_io_FileCleanable_cleanupClose0(JNIEnv *env, jclass fdClass, jint unused, jlong handle) {
    if (handle != -1) {
        if (!CloseHandle((HANDLE)handle)) {
            JNU_ThrowIOExceptionWithLastError(env, "close failed");
        }
    }
}

JNIEXPORT jstring JNICALL
Java_java_io_FileDescriptor_nativeDescription0(JNIEnv* env, jobject this) {
    HANDLE handle = (HANDLE)(*env)->GetLongField(env, this, IO_handle_fdID);
    #define BufferSize 1024
    char lpszFilePath[BufferSize] = {'\0'};

    HMODULE hKernelDll = LoadLibrary(TEXT("kernel32.dll"));
    if (!hKernelDll) {
        JNU_ThrowIOExceptionWithLastError(env, "LoadLibrary kernel32.dll failed");
        return NULL;
    }

    HMODULE hNtdllDll = GetModuleHandle(TEXT("ntdll.dll"));
    if (!hNtdllDll) {
        JNU_ThrowIOExceptionWithLastError(env, "LoadLibrary ntdll.dll failed");
        CloseHandle(hKernelDll);
        return NULL;
    }

    typedef NTSTATUS(WINAPI* NtQueryObjectFunc)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    typedef BOOL(WINAPI* GetFinalPathNameByHandleFunc)(HANDLE, LPSTR, DWORD, DWORD);

    NtQueryObjectFunc ntQueryObject = (NtQueryObjectFunc)GetProcAddress(hNtdllDll, "NtQueryObject");
    GetFinalPathNameByHandleFunc getFinalPathNameByHandle = (GetFinalPathNameByHandleFunc)GetProcAddress(hKernelDll, "GetFinalPathNameByHandleA");

    if (!ntQueryObject || !getFinalPathNameByHandle) {
        JNU_ThrowIOExceptionWithLastError(env, "GetProcAddress failed");
    } else {
        char tmp[BufferSize];
        PUBLIC_OBJECT_TYPE_INFORMATION *objTypeInfo = (PUBLIC_OBJECT_TYPE_INFORMATION *)tmp;
        ULONG retLen;
        NTSTATUS status = ntQueryObject(handle, ObjectTypeInformation, objTypeInfo, sizeof(tmp), &retLen);
        if (0 != status) {
            JNU_ThrowIOExceptionWithLastError(env, "NtQueryObject failed");
        } else {
            PCWSTR typeName = objTypeInfo->TypeName.Buffer;
            const BOOL hasFilepath = (0 == wcscmp(L"File", typeName) || 0 == wcscmp(L"Directory", typeName));
            if (!hasFilepath || !getFinalPathNameByHandle(handle, lpszFilePath, BufferSize, FILE_NAME_OPENED)) {
                int len = snprintf(lpszFilePath, sizeof(lpszFilePath) - 1, "Handle %p, ", handle);
                if (0 > len) {
                    len = 0;
                }
                WideCharToMultiByte(CP_ACP, 0, typeName, -1, lpszFilePath + len, sizeof(lpszFilePath) - len, NULL, NULL);
            }
        }
    }

    CloseHandle(hNtdllDll);
    CloseHandle(hKernelDll);

    return (*env)->NewStringUTF(env, lpszFilePath);
}
