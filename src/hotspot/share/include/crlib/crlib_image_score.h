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
#ifndef CRLIB_IMAGE_SCORE_H
#define CRLIB_IMAGE_SCORE_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_IMAGE_SCORE_NAME "image score"
#define CRLIB_EXTENSION_IMAGE_SCORE(api) \
  CRLIB_EXTENSION(api, crlib_image_score_t, CRLIB_EXTENSION_IMAGE_SCORE_NAME)

// API for quantifying image performance. This is a write-only API.
typedef const struct crlib_image_score {
  crlib_extension_t header;
  // Invoked before checkpoint. When invoked with the same metric name
  // multiple times the older value is overwritten.
  // Returns false if the score cannot be recorded, true on success.
  // The score is persisted during checkpoint (not in this function).
  bool (*set_score)(crlib_conf_t *conf, const char *metric, double value);
} crlib_image_score_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_IMAGE_SCORE_H
