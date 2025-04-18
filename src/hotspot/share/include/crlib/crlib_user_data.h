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
#ifndef CRLIB_USER_DATA_H
#define CRLIB_USER_DATA_H
#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_FEATURE_USER_DATA_NAME "user_data"
#define CRLIB_FEATURE_USER_DATA(api) CRLIB_FEATURE(api, crlib_user_data_t, CRLIB_FEATURE_USER_DATA_NAME)

typedef const struct crlib_user_data crlib_user_data_t;
typedef struct crlib_user_data_list crlib_user_data_list_t;

// API for recording of USER_DATA-aware information
struct crlib_user_data {
    crlib_feature_t header;

    // Optionally sets arbitrary user data to store in an ELF note during checkpoint. Returns success.
    // Passed memory is no longer referenced. Caller has to call checkpoint afterwards which will free the memory.
    bool (*set_user_data)(crlib_conf_t *, const char *name, const void *data, size_t size);

    // Optionally loads user data from conf's image location.
    // Returns NULL on error.
    // Caller has to call free_user_data afterwards; even if it returned false.
    crlib_user_data_list_t *(*load_user_data)(crlib_conf_t *);

    // Find user data of name and store it to *data_p and *size_p. Returns success.
    // Stored memory must not be freed. It is no longer valid after free_user_data.
    bool (*lookup_user_data)(crlib_user_data_list_t *user_data, const char *name, const void **data_p, size_t *size_p);

    // Free memory allocated by load_user_data.
    void (*free_user_data)(crlib_user_data_list_t *user_data);
};

#ifdef __cplusplus
} // extern "C"
#endif
#endif // CRLIB_USER_DATA_H
