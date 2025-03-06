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
#ifndef CRLIB_DESCRIPTION_H
#define CRLIB_DESCRIPTION_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_DESCRIPTION_NAME "description"
#define CRLIB_EXTENSION_DESCRIPTION(api) \
  CRLIB_EXTENSION(api, crlib_description_t, CRLIB_EXTENSION_DESCRIPTION_NAME)

// API for obtaining engine description.
//
// Unless noted otherwise, storage duration of the returned data should (1) be either static or
// tied to the storage duration of conf, (2) not change between calls with the same arguments.
struct crlib_description {
  crlib_extension_t header;

  // Returns a valid C-string containing concise information about the engine, e.g. its name and
  // version, or null on error.
  const char *(*identity)(crlib_conf_t *);
  // Returns a valid C-string containing a short user-friendly description of the engine, or null
  // on error.
  const char *(*description)(crlib_conf_t *);

  // Returns a valid C-string with a formatted list of configuration keys supported by the engine
  // with their descriptions, or null on error.
  //
  // Some keys can be excluded if they are not supposed to be set by a user but rather by the
  // application the engine is linked to.
  //
  // Example:
  // "
  // * do_stuff=<true/false> (default: true) — whether to do stuff.\n
  // * args=<string> (default: \"\") — other arguments.
  // "
  const char *(*configuration_doc)(crlib_conf_t *);

  // Returns a null-terminated array of all configuration keys supported by the engine, or null if
  // this method is not supported.
  const char * const *(*configurable_keys)(crlib_conf_t *);
  // Returns a null-terminated array of all API extensions supported by the engine, or null if this
  // method is not supported.
  crlib_extension_t * const *(*supported_extensions)(crlib_conf_t *);
};
typedef const struct crlib_description crlib_description_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_DESCRIPTION_H
