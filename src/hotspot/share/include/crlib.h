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
#ifndef CRLIB_H
#define CRLIB_H

#include <stdbool.h>
#include <stddef.h>

struct _crlib_api;
typedef struct _crlib_api crlib_api;

typedef bool (*checkpoint_func_t)(crlib_api *api);
typedef void (*restore_func_t)(crlib_api *api);

struct _crlib_api {
    // Function called to trigger the native checkpoint.
    // Set in the CRLIB_API_INIT function.
    checkpoint_func_t checkpoint;
    // Function called to trigger the native restore.
    // Set in the CRLIB_API_INIT function.
    restore_func_t restore;

    // Path for additional dynamic libraries or executables if needed by the implementation.
    // Set by calling application.
    const char *library_path;
    // Target location for checkpoint/source location for restore. Commonly this is a path
    // to directory in the local filesystem.
    // Set by calling application.
    const char *image_location;
    // Any additional parameters for the native checkpoint/restore.
    // Set by calling application.
    const char *args;
    // Identifier for the shared memory used by CRaC to pass VM Options,
    // environment and system properties from the restoring to the restored process.
    // Set by the restoring process, read by the restored process.
    int shmid;
    // Keep the checkpointed application running after checkpoint. By default the process
    // is killed with SIGKILL.
    bool leave_running;
};

#define CRLIB_API_INIT crlib_api_init
#define CRLIB_API_INIT_FUNC "crlib_api_init"

#define CRLIB_API_VERSION 1

typedef bool (*init_api_func_t)(int api_version, crlib_api *api, size_t api_size);

extern bool CRLIB_API_INIT(int api_version, crlib_api *api, size_t api_size);

#endif // CRLIB_H
