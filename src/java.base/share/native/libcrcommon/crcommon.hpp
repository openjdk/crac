/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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
#ifndef CRCOMMON_HPP
#define CRCOMMON_HPP

#include <cstdio>
#include <utility>

#include "jni.h"
#include "crlib/crlib.h"
#include "crlib/crlib_image_constraints.h"
#include "crlib/crlib_image_score.h"

#ifdef STATIC_BUILD
# define CONCAT_2(a, b) a##b
# define CONCAT(a, b) CONCAT_2(a, b)
# define CRLIB_API_MAYBE_STATIC CONCAT(CONCAT(CRLIB_API, _), LIBRARY_NAME)
#else
# define CRLIB_API_MAYBE_STATIC CRLIB_API
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

#define LOG(fmt, ...) fprintf(stderr, "%s: " fmt "\n", crcommon_log_prefix(), ##__VA_ARGS__)

// For Windows
#if !defined(PATH_MAX) && defined(MAX_PATH)
# define PATH_MAX MAX_PATH
#elif !defined(PATH_MAX)
# define PATH_MAX 1024
#endif

template<typename F> class Deferred;
template<typename F> inline Deferred<F> defer(F&& f);

template<typename F> class Deferred {
friend Deferred<F> defer<F>(F&& f);
private:
  F _f;
  inline explicit Deferred(F f): _f(f) {}
public:
  inline ~Deferred() { _f(); }
};

template<typename F> inline Deferred<F> defer(F&& f) {
  return Deferred<F>(std::forward<F>(f));
}

typedef struct crcommon crcommon_t;
typedef crcommon_t* (crlib_to_crcommon_t)(crlib_conf_t*);

#ifdef CRCOMMON_IMPL
# define CRCOMMON_API JNIEXPORT
#else
# define CRCOMMON_API JNIIMPORT
#endif

extern "C" {
  extern CRCOMMON_API crcommon_t* crcommon_create(const char* log_prefix);
  extern CRCOMMON_API void crcommon_destroy(crcommon_t* conf);
  extern CRCOMMON_API const char* crcommon_log_prefix();

  extern CRCOMMON_API crlib_image_constraints_t image_constraints_extension;
  extern CRCOMMON_API bool image_constraints_persist(const crcommon_t* conf, const char* image_location);
  extern CRCOMMON_API bool image_constraints_validate(const crcommon_t* conf, const char* image_location);

  extern CRCOMMON_API crlib_image_score_t image_score_extension;
  extern CRCOMMON_API bool image_score_persist(const crcommon_t* conf, const char* image_location);
  extern CRCOMMON_API void image_score_reset(crcommon_t* conf);

  // helper function
  extern CRCOMMON_API crlib_extension_t* find_extension(crlib_extension_t* const* extensions, const char* name, size_t size);
}

#undef CRCOMMON_API

typedef struct crlib_base {
protected:
  crcommon_t* _common;

  crlib_base(const char* log_prefix): _common(crcommon_create(log_prefix)) {}
  ~crlib_base() {
    crcommon_destroy(_common);
  }

public:
  inline crcommon_t* common() { return _common; }
} crlib_base_t;

// We cannot use simply 'struct crlib_conf' for engine implementation;
// in static build the symbols would mix with other implementations
#define RENAME_CRLIB(other_class) \
  struct crlib_conf: other_class { \
    crlib_conf() = delete; \
    ~crlib_conf() = delete; \
  }

#endif // CRCOMMON_HPP
