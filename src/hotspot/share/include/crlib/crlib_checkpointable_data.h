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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
#ifndef CRLIB_CHECKPOINTABLE_DATA_H
#define CRLIB_CHECKPOINTABLE_DATA_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_CHECKPOINTABLE_DATA_NAME "checkpointable data"
#define CRLIB_EXTENSION_CHECKPOINTABLE(api) \
  CRLIB_EXTENSION(api, crlib_checkpointable_data_t, CRLIB_EXTENSION_CHECKPOINTABLE_DATA_NAME)

typedef enum checkpointable_status {
    never,      // it's not able to commit checkpoint
    ready,      // checkpointable
    ready_later // at some point it could become checkpointable
} checkpointable_status_t;

// API for obtaining information about chackpointable status.
struct crlib_checkpointable_data {
  crlib_extension_t header;

  checkpointable_status_t (*get_checkpointable_status)(crlib_conf_t *);
};
typedef const struct crlib_checkpointable_data crlib_checkpointable_data_t;

#ifdef __cplusplus
} // extern "C
#endif

#endif // CRLIB_CHECKPOINTABLE_DATA_H