/*
 * Copyright (c) 2023, 2026, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#ifndef USER_DATA_HPP
#define USER_DATA_HPP

#include "crlib/crlib_user_data.h"

class UserData;
struct user_data_chunk;

struct crlib_user_data_storage {
  UserData *user_data;
  struct user_data_chunk *chunk;
};

class UserData {
private:
    const char **_image_location;

    const char *image_location() {
        return *_image_location;
    }
public:
    explicit UserData(const char **image_location): _image_location(image_location) {}

    bool set_user_data(const char *name, const void *data, size_t size);
    crlib_user_data_storage_t *load_user_data();
    bool lookup_user_data(crlib_user_data_storage_t *user_data, const char *name, const void **data_p, size_t *size_p);
    void destroy_user_data(crlib_user_data_storage_t *user_data);
};

#endif // USER_DATA_HPP
