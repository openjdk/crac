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
#ifndef CRLIB_IMAGE_CONSTRAINTS_H
#define CRLIB_IMAGE_CONSTRAINTS_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME "image constraints"
#define CRLIB_EXTENSION_IMAGE_CONSTRAINTS(api) \
  CRLIB_EXTENSION(api, crlib_image_constraints_t, CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME)

typedef enum {
  /* Natural zero-extensions of the bitmaps are equal */
  EQUALS,
  /* Bitmap in image must be subset or equal to bitmap in constraint */
  SUBSET,
  /* Bitmap in image must be superset or equal to bitmap in constraint */
  SUPERSET,
} bitmap_comparison_t;

// API for storing & verifying application-defined labels and bitmaps
typedef struct crlib_image_constraints {
  crlib_extension_t header;

  // Invoked before checkpoint. Return false if name or value exceed limits, or if the name was already used.
  bool (*set_label)(crlib_conf_t *, const char *name, const char *value);
  bool (*set_bitmap)(crlib_conf_t *, const char *name, const unsigned char *value, size_t length_bytes);

  // Invoked before restore. The conditions are not evaluated immediately; the restore will fail
  // with RESTORE_ERROR_INVALID if these constraints are not matched.
  void (*require_label)(crlib_conf_t *, const char *name, const char *value);
  void (*require_bitmap)(crlib_conf_t *, const char *name, const unsigned char *value, size_t length_bytes, bitmap_comparison_t comparison);
} crlib_image_constraints_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_USER_DATA_H
