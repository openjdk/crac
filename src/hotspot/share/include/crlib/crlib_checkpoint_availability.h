/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
#ifndef CRLIB_CHECKPOINT_AVAILABILITY_H
#define CRLIB_CHECKPOINT_AVAILABILITY_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_CHECKPOINT_AVAILABILITY_NAME "checkpointable data"
#define CRLIB_EXTENSION_CHECKPOINTABLE(api) \
  CRLIB_EXTENSION(api, crlib_checkpoint_availability_t, CRLIB_EXTENSION_CHECKPOINT_CRLIB_CHECKPOINT_AVAILABILITY_NAME)

typedef int crlib_checkpointable_status_t;
#define CRLIB_CHECKPOINTABLE_NEVER   0   // engine will never accept another checkpoint
#define CRLIB_CHECKPOINTABLE_NOT_YET 1   // not now; may become ready later
#define CRLIB_CHECKPOINTABLE_READY   2   // checkpoint can proceed


// API for obtaining information about chackpointable status.
struct crlib_checkpoint_availability {
  crlib_extension_t header;

  crlib_checkpointable_status_t (*get_checkpointable_status)(crlib_conf_t *);
};
typedef const struct crlib_checkpoint_availability crlib_checkpoint_availability_t;

#ifdef __cplusplus
} // extern "C
#endif

#endif // CRLIB_CHECKPOINT_AVAILABILITY_H