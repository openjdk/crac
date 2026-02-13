/*
 * Copyright (c) 2025, 2026, Azul Systems, Inc. All rights reserved.
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
#ifndef ENVIRONMENT_HPP
#define ENVIRONMENT_HPP

#include <stddef.h>

#include "crcommon.hpp"

// crexec_md.cpp
char **get_environ();

class Environment {
private:
  char **_env;
  size_t _length = 0;

public:
  explicit Environment(const char * const *env = get_environ());
  ~Environment();

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _env != nullptr; }

  char **env() { return _env; }

  bool append(const char *var, const char *value);
  bool add_criu_option(const char *opt);
};

#endif // ENVIRONMENT_HPP
