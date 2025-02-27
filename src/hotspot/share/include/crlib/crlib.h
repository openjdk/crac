/*
 * Copyright (c) 2023-2025, Azul Systems, Inc. All rights reserved.
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

#ifdef __cplusplus
extern "C" {
#endif

// Configuration storage used by CRaC engine to persist data between API calls.
typedef struct crlib_conf crlib_conf_t;

// The first member in an actual structure defining an extension of CRaC engine API.
typedef struct crlib_extension {
  // Name of the extension.
  // If there is a non-backwards compatible change in the extension (from API point of view) the
  // name should be changed, e.g. foo -> foo:v2.
  const char *name;
  // Size of the full extension structure, in bytes.
  // Adding members to the end of the full structure is considered a backwards-compatible change.
  size_t size;
} crlib_extension_t;

// CRaC engine API.
//
// Unless noted otherwise, the engine should copy data passed through these methods into the
// configuration storage if it needs to keep it.
struct crlib_api {
  // Initializes a configuration structure.
  crlib_conf_t *(*create_conf)();
  // Destroys a configuration structure. The argument can be null.
  void (*destroy_conf)(crlib_conf_t *);

  // Triggers a checkpoint. Returns zero on success.
  int (*checkpoint)(crlib_conf_t *);
  // Triggers a restore. Does not normally return, but if it does returns a error code.
  int (*restore)(crlib_conf_t *);

  // Returns true if the given configuration key is supported by the engine, false otherwise.
  // Key is a valid C-string.
  // Use of this before configuring is not a requirement.
  bool (*can_configure)(crlib_conf_t *, const char *key);
  // Sets a configuration option. Returns true on success.
  // Key and value are valid C-strings.
  bool (*configure)(crlib_conf_t *, const char *key, const char *value);

  // Returns an API extension with the given name (C-string) and size, or null if an extension with
  // such name is not present or its size is lower than requested.
  // The extension should have static storage duration. The application is supposed to cast it to
  // the actual extension type.
  const crlib_extension_t *(*get_extension)(const char *name, size_t size);
};
typedef const struct crlib_api crlib_api_t;

#define CRLIB_API crlib_api
#define CRLIB_API_FUNC "crlib_api"

#define CRLIB_API_VERSION 2

#if defined(WINDOWS) || defined(_WINDOWS)
  #ifdef CRLIB_IS_IMPL
    #define IMPORTEXPORT __declspec(dllexport)
  #else
    #define IMPORTEXPORT __declspec(dllimport)
  #endif
#else
  #define IMPORTEXPORT
#endif

// Returns a CRaC API of the given version and size, or null if such API version is not supported
// or its size is lower than requested.
// The API should have static storage duration.
extern IMPORTEXPORT crlib_api_t *CRLIB_API(int api_version, size_t api_size);

#define CRLIB_EXTENSION(api, type, name) ((type *) (api)->get_extension(name, sizeof(type)))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_H
