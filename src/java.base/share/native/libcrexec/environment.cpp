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
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "crcommon.hpp"
#include "environment.hpp"

Environment::Environment(const char * const *env) {
  for (; env[_length] != nullptr; _length++) {}

  // Not using new here because we cannot safely use realloc with it
  _env = static_cast<char**>(malloc((_length + 1) * sizeof(char *)));
  if (_env == nullptr) {
    return;
  }

  for (size_t i = 0; i < _length; i++) {
    _env[i] = strdup(env[i]);
    if (_env[i] == nullptr) {
      for (size_t j = 0; j < i; i++) {
        free(_env[j]);
        free(_env);
        _env = nullptr;
      }
      assert(!is_initialized());
      return;
    }
  }
  _env[_length] = nullptr;
}

Environment::~Environment() {
  if (is_initialized()) {
    for (size_t i = 0; i < _length; i++) {
      free(_env[i]);
    }
    free(_env);
  }
}


bool Environment::append(const char *var, const char *value) {
  assert(is_initialized());

  const size_t str_size = strlen(var) + strlen("=") + strlen(value) + 1;
  char * const str = static_cast<char *>(malloc(sizeof(char) * str_size));
  if (str == nullptr) {
    LOG("out of memory");
    return false;
  }
  if (snprintf(str, str_size, "%s=%s", var, value) != static_cast<int>(str_size) - 1) {
    LOG("snprintf env var: %s", strerror(errno));
    free(str);
    return false;
  }

  {
    char ** const new_env = static_cast<char **>(realloc(_env, (_length + 2) * sizeof(char *)));
    if (new_env == nullptr) {
      LOG("out of memory");
      free(str);
      return false;
    }
    _env = new_env;
  }

  _env[_length++] = str;
  _env[_length] = nullptr;

  return true;
}

bool Environment::add_criu_option(const char *opt) {
  constexpr char CRAC_CRIU_OPTS[] = "CRAC_CRIU_OPTS";
  constexpr size_t CRAC_CRIU_OPTS_LEN = ARRAY_SIZE(CRAC_CRIU_OPTS) - 1;

  assert(is_initialized());

  bool opts_found = false;
  size_t opts_index = 0;
  for (; _env[opts_index] != nullptr; opts_index++) {
    if (strncmp(_env[opts_index], CRAC_CRIU_OPTS, CRAC_CRIU_OPTS_LEN) == 0 &&
        _env[opts_index][CRAC_CRIU_OPTS_LEN] == '=') {
      opts_found = true;
      break;
    }
  }

  if (!opts_found) {
    return append(CRAC_CRIU_OPTS, opt);
  }

  if (strstr(_env[opts_index] + CRAC_CRIU_OPTS_LEN + 1, opt) != nullptr) {
    return true;
  }

  const size_t new_opts_size = strlen(_env[opts_index]) + strlen(" ") + strlen(opt) + 1;
  char * const new_opts = static_cast<char *>(malloc(new_opts_size * sizeof(char)));
  if (new_opts == nullptr) {
    LOG("out of memory");
    return false;
  }
  if (snprintf(new_opts, new_opts_size, "%s %s", _env[opts_index], opt) !=
        static_cast<int>(new_opts_size) - 1) {
    LOG("snprintf CRAC_CRIU_OPTS (append): %s", strerror(errno));
    free(new_opts);
    return false;
  }
  free(_env[opts_index]);
  _env[opts_index] = new_opts;

  return true;
}
