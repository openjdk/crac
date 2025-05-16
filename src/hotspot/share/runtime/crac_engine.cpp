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

#include "precompiled.hpp"

#include "crlib/crlib.h"
#include "crlib/crlib_restore_data.h"
#include "crlib/crlib_user_data.h"
#include "logging/log.hpp"
#include "memory/allStatic.hpp"
#include "nmt/memTag.hpp"
#include "runtime/crac_engine.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/macros.hpp"
#include "utilities/resourceHash.hpp"

#include <cstddef>
#include <cstring>

// CRaC engine configuration options JVM sets directly instead of relaying from the user
#define VM_CONTROLLED_ENGINE_OPTS(OPT) \
  OPT(image_location) \
  OPT(exec_location) \

#define ARRAY_ELEM(opt) #opt,
static constexpr const char * const vm_controlled_engine_opts[] = {
  VM_CONTROLLED_ENGINE_OPTS(ARRAY_ELEM) nullptr
};
#undef ARRAY_ELEM

#define DEFINE_OPT_VAR(opt) static constexpr const char engine_opt_##opt[] = #opt;
VM_CONTROLLED_ENGINE_OPTS(DEFINE_OPT_VAR)
#undef DEFINE_OPT_VAR

#ifdef _WINDOWS
static char *strsep(char **strp, const char *delim) {
  char *str = *strp;
  if (str == nullptr) {
    return nullptr;
  }
  size_t len = strcspn(str, delim);
  if (str[len] == '\0') {
    *strp = nullptr;
    return str;
  }
  str[len] = '\0';
  *strp += len + 1;
  return str;
}
#endif // _WINDOWS

const char * const *CracEngine::vm_controlled_options() {
  return vm_controlled_engine_opts;
}

static bool find_engine(const char *dll_dir, char *path, size_t path_size, bool *is_library) {
  // Try to interpret as a file path
  if (os::is_path_absolute(CRaCEngine)) {
    const size_t path_len = strlen(CRaCEngine);
    if (path_len + 1 > path_size) {
      log_error(crac)("CRaCEngine file path is too long: %s", CRaCEngine);
      return false;
    }

    if (!os::file_exists(CRaCEngine)) {
      log_error(crac)("CRaCEngine file does not exist: %s", CRaCEngine);
      return false;
    }

    strcpy(path, CRaCEngine);

    const char *last_slash = strrchr(CRaCEngine, *os::file_separator());
    const char *basename;
    if (last_slash == nullptr) {
      basename = CRaCEngine;
    } else {
      basename = last_slash + strlen(os::file_separator());
    }
    *is_library = strncmp(basename, JNI_LIB_PREFIX, strlen(JNI_LIB_PREFIX)) == 0 &&
      strcmp(path + path_len - strlen(JNI_LIB_SUFFIX), JNI_LIB_SUFFIX) == 0;
    log_debug(crac)("CRaCEngine path %s is %s library", CRaCEngine, *is_library ? "a" : "not a");

    return true;
  }

  // Try to interpret as a library name
  if (os::dll_locate_lib(path, path_size, dll_dir, CRaCEngine)) {
    *is_library = true;
    log_debug(crac)("Found CRaCEngine %s as a library in %s", CRaCEngine, path);
    return true;
  }

  *is_library = false;
  log_debug(crac)("CRaCEngine %s is not a library in %s", CRaCEngine, dll_dir);

  constexpr const char suffix[] = WINDOWS_ONLY(".exe") NOT_WINDOWS("");
#ifndef S_ISREG
# define S_ISREG(__mode) (((__mode) & S_IFMT) == S_IFREG)
#endif // S_ISREG
  struct stat st;

  // Try to interpret as an executable name with "engine" suffix omitted
  size_t path_len = strlen(dll_dir) + strlen(os::file_separator()) + strlen(CRaCEngine) + strlen("engine") + strlen(suffix);
  if (path_len + 1 <= path_size) {
    os::snprintf_checked(path, path_size, "%s%s%sengine%s", dll_dir, os::file_separator(), CRaCEngine, suffix);
    if (os::stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      log_debug(crac)("Found CRaCEngine %s as %s", CRaCEngine, path);
      return true;
    }
  } else {
    log_debug(crac)("Not looking for CRaCEngine an executable name with 'engine' omitted: path is too long");
  }

  // Try to interpret as an executable name
  precond(path_len > strlen("engine"));
  path_len -= strlen("engine");
  if (path_len + 1 <= path_size) {
    os::snprintf_checked(path, path_size, "%s%s%s%s", dll_dir, os::file_separator(), CRaCEngine, suffix);
    if (os::stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      log_debug(crac)("Found CRaCEngine %s as %s", CRaCEngine, path);
      return true;
    }
  } else {
    log_debug(crac)("Not looking for CRaCEngine as an executable name: path is too long");
  }

  return false;
}

static bool configure_image_location(const crlib_api_t &api, crlib_conf_t *conf, const char *image_location) {
  precond(image_location != nullptr && image_location[0] != '\0');
  if (!api.configure(conf, engine_opt_image_location, image_location)) {
    log_error(crac)("CRaC engine failed to configure: '%s' = '%s'", engine_opt_image_location, image_location);
    return false;
  }
  return true;
}

// These functions are used in a template instantiation and need to have external linkage. Otherwise
// Windows-debug build fails with linkage errors for the instantiation's symbols.
class CStringUtils : public AllStatic {
public:
  static unsigned int hash(const char * const &s) {
    unsigned int h = 0;
    for (const char *p = s; *p != '\0'; p++) {
      h = 31 * h + *p;
    }
    return h;
  }

  static bool equals(const char * const &s0, const char * const &s1) {
    return strcmp(s0, s1) == 0;
  }
};

// Have to use C-heap because resource area may yet be unavailable when this is used
using CStringSet = ResourceHashtable<const char *, bool, 256, AnyObj::C_HEAP, MemTag::mtInternal,
                                     CStringUtils::hash, CStringUtils::equals>;

static crlib_conf_t *create_conf(const crlib_api_t &api, const char *image_location, const char *exec_location) {
  crlib_conf_t * const conf = api.create_conf();
  if (conf == nullptr) {
    log_error(crac)("CRaC engine failed to create its configuration");
    return nullptr;
  }

  if (CRaCEngineOptions != nullptr && strcmp(CRaCEngineOptions, "help") == 0) {
    return conf;
  }

  if (image_location != nullptr && !configure_image_location(api, conf, image_location)) {
    api.destroy_conf(conf);
    return nullptr;
  }

  if (exec_location != nullptr) { // Only passed when using crexec
    guarantee(api.can_configure(conf, engine_opt_exec_location),
              "crexec does not support expected option: %s", engine_opt_exec_location);
    if (!api.configure(conf, engine_opt_exec_location, exec_location)) {
      log_error(crac)("crexec failed to configure: '%s' = '%s'", engine_opt_exec_location, exec_location);
      api.destroy_conf(conf);
      return nullptr;
    }
  }

  if (CRaCEngineOptions == nullptr || CRaCEngineOptions[0] == '\0' /* possible for ccstrlist */) {
    return conf;
  }

  CStringSet vm_controlled_keys;
#define PUT_CONTROLLED_KEY(opt) vm_controlled_keys.put_when_absent(engine_opt_##opt, false);
  VM_CONTROLLED_ENGINE_OPTS(PUT_CONTROLLED_KEY)
#undef PUT_CONTROLLED_KEY

  char *engine_options = os::strdup_check_oom(CRaCEngineOptions, mtInternal);
  char *const engine_options_start = engine_options;
  CStringSet keys;
  do {
    char *key_value = strsep(&engine_options, ",\n"); // '\n' appears when ccstrlist is appended to
    const char *key = strsep(&key_value, "=");
    const char *value = key_value != nullptr ? key_value : "";
    assert(key != nullptr, "Should have terminated before");
    if (vm_controlled_keys.contains(key)) {
      log_warning(crac)("VM-controlled CRaC engine option provided, skipping: %s", key);
      continue;
    }
    {
      bool is_new_key;
      keys.put_if_absent(key, &is_new_key);
      if (!is_new_key) {
        log_warning(crac)("CRaC engine option '%s' specified multiple times", key);
      }
    }
    if (!api.configure(conf, key, value)) {
      log_error(crac)("CRaC engine failed to configure: '%s' = '%s'", key, value);
      os::free(engine_options_start);
      api.destroy_conf(conf);
      return nullptr;
    }
    log_debug(crac)("CRaC engine option: '%s' = '%s'", key, value);
  } while (engine_options != nullptr);
  os::free(engine_options_start);

  return conf;
}

CracEngine::CracEngine(const char *image_location) {
  if (CRaCEngine == nullptr) {
    log_error(crac)("CRaCEngine must not be empty");
    return;
  }

  // Arguments::get_dll_dir() might not have been initialized yet
  char dll_dir[JVM_MAXPATHLEN];
  os::jvm_path(dll_dir, sizeof(dll_dir));
  // path is ".../lib/server/libjvm.so", or "...\bin\server\libjvm.dll"
  char *after_elem = nullptr;
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(dll_dir, *os::file_separator());
    *after_elem = '\0';
  }

  char path[JVM_MAXPATHLEN];
  bool is_library;
  if (!find_engine(dll_dir, path, sizeof(path), &is_library)) {
    log_error(crac)("Cannot find CRaC engine %s", CRaCEngine);
    return;
  }
  postcond(path[0] != '\0');

  char exec_path[JVM_MAXPATHLEN] = "\0";
  if (!is_library) {
    strcpy(exec_path, path); // Save to later pass it to crexec
    if (!os::dll_locate_lib(path, sizeof(path), dll_dir, "crexec")) {
      log_error(crac)("Cannot find crexec library to use CRaCEngine executable");
      return;
    }
  }

  char error_buf[1024];
  void * const lib = os::dll_load(path, error_buf, sizeof(error_buf));
  if (lib == nullptr) {
    log_error(crac)("Cannot load CRaC engine library from %s: %s", path, error_buf);
    return;
  }

  using api_func_t = decltype(&CRLIB_API);
  const auto api_func = reinterpret_cast<api_func_t>(os::dll_lookup(lib, CRLIB_API_FUNC));
  if (api_func == nullptr) {
    log_error(crac)("Cannot load CRaC engine library entrypoint '" CRLIB_API_FUNC "' from %s", path);
    os::dll_unload(lib);
    return;
  }

  crlib_api_t * const api = api_func(CRLIB_API_VERSION, sizeof(crlib_api_t));
  if (api == nullptr) {
    log_error(crac)("CRaC engine failed to initialize its API (version %i). "
                    "Maybe this version is not supported?", CRLIB_API_VERSION);
    os::dll_unload(lib);
    return;
  }
  if (api->create_conf == nullptr || api->destroy_conf == nullptr ||
      api->checkpoint == nullptr || api->restore == nullptr ||
      api->can_configure == nullptr || api->configure == nullptr ||
      api->get_extension == nullptr) {
    log_error(crac)("CRaC engine provided invalid API");
    os::dll_unload(lib);
    return;
  }

  const char *exec_location = exec_path[0] != '\0' ? exec_path : nullptr;
  crlib_conf_t * const conf = create_conf(*api, image_location, exec_location);
  if (conf == nullptr) {
    os::dll_unload(lib);
    return;
  }

  _lib = lib;
  _api = api;
  _conf = conf;
}

CracEngine::~CracEngine() {
  if (is_initialized()) {
    _api->destroy_conf(_conf);
    os::dll_unload(_lib);
  }
}

bool CracEngine::is_initialized() const {
  assert((_lib == nullptr && _api == nullptr && _conf == nullptr) ||
          (_lib != nullptr && _api != nullptr && _conf != nullptr), "invariant");
  return _lib != nullptr;
}

int CracEngine::checkpoint() const {
  precond(is_initialized());
  return _api->checkpoint(_conf);
}

int CracEngine::restore() const {
  precond(is_initialized());
  return _api->restore(_conf);
}

bool CracEngine::configure_image_location(const char *image_location) const {
  precond(is_initialized());
  return ::configure_image_location(*_api, _conf, image_location);
}

CracEngine::ApiStatus CracEngine::prepare_restore_data_api() {
  precond(is_initialized());
  if (_restore_data_api != nullptr) {
    return ApiStatus::OK;
  }

  crlib_restore_data_t * const restore_data_api = CRLIB_EXTENSION_RESTORE_DATA(_api);
  if (restore_data_api == nullptr) {
    log_debug(crac)("CRaC engine does not support extension: " CRLIB_EXTENSION_RESTORE_DATA_NAME);
    return ApiStatus::UNSUPPORTED;
  }
  if (restore_data_api->set_restore_data == nullptr || restore_data_api->get_restore_data == nullptr) {
    log_error(crac)("CRaC engine provided invalid API for extension: " CRLIB_EXTENSION_RESTORE_DATA_NAME);
    return ApiStatus::ERR;
  }
  _restore_data_api = restore_data_api;
  return ApiStatus::OK;
}

bool CracEngine::set_restore_data(const void *data, size_t size) const {
  precond(_restore_data_api != nullptr);
  return _restore_data_api->set_restore_data(_conf, data, size);
}

size_t CracEngine::get_restore_data(void *buf, size_t size) const {
  precond(_restore_data_api != nullptr);
  return _restore_data_api->get_restore_data(_conf, buf, size);
}

CracEngine::ApiStatus CracEngine::prepare_description_api() {
  precond(is_initialized());
  if (_description_api != nullptr) {
    return ApiStatus::OK;
  }

  crlib_description_t * const description_api = CRLIB_EXTENSION_DESCRIPTION(_api);
  if (description_api == nullptr) {
    log_debug(crac)("CRaC engine does not support extension: " CRLIB_EXTENSION_DESCRIPTION_NAME);
    return ApiStatus::UNSUPPORTED;
  }
  if (description_api->identity == nullptr || description_api->description == nullptr ||
      description_api->configuration_doc == nullptr ||
      description_api->configurable_keys == nullptr ||
      description_api->supported_extensions == nullptr) {
    log_error(crac)("CRaC engine provided invalid API for extension: " CRLIB_EXTENSION_DESCRIPTION_NAME);
    return ApiStatus::ERR;
  }
  _description_api = description_api;
  return ApiStatus::OK;
}

const char *CracEngine::description() const {
  precond(_description_api != nullptr);
  return _description_api->description(_conf);
}

const char *CracEngine::configuration_doc() const {
  precond(_description_api != nullptr);
  return _description_api->configuration_doc(_conf);
}

static constexpr char cpufeatures_userdata_name[] = "cpufeatures";

CracEngine::ApiStatus CracEngine::prepare_user_data_api() {
  precond(is_initialized());
  if (_user_data_api != nullptr) {
    return ApiStatus::OK;
  }

  crlib_user_data_t * const user_data_api = CRLIB_EXTENSION_USER_DATA(_api);
  if (user_data_api == nullptr) {
    log_debug(crac)("CRaC engine does not support extension: " CRLIB_EXTENSION_USER_DATA_NAME);
    return ApiStatus::UNSUPPORTED;
  }
  if (user_data_api->set_user_data == nullptr || user_data_api->load_user_data == nullptr
      || user_data_api->lookup_user_data == nullptr || user_data_api->destroy_user_data == nullptr) {
    log_error(crac)("CRaC engine provided invalid API for extension: " CRLIB_EXTENSION_USER_DATA_NAME);
    return ApiStatus::ERR;
  }
  _user_data_api = user_data_api;
  return ApiStatus::OK;
}

// Return success.
bool CracEngine::cpufeatures_store(const VM_Version::CPUFeaturesBinary *datap) const {
  log_debug(crac)("cpufeatures_store user data %s to %s...", cpufeatures_userdata_name, CRaCRestoreFrom);
  const bool ok = _user_data_api->set_user_data(_conf, cpufeatures_userdata_name, datap, sizeof(*datap));
  if (!ok) {
    log_error(crac)("CRaC engine failed to store user data %s", cpufeatures_userdata_name);
  }
  return ok;
}

// Return success.
bool CracEngine::cpufeatures_load(VM_Version::CPUFeaturesBinary *datap, bool *presentp) const {
  log_debug(crac)("cpufeatures_load user data %s from %s...", cpufeatures_userdata_name, CRaCRestoreFrom);
  crlib_user_data_storage_t *user_data;
  if (!(user_data = _user_data_api->load_user_data(_conf))) {
    log_error(crac)("CRaC engine failed to load user data %s", cpufeatures_userdata_name);
    return false;
  }
  const VM_Version::CPUFeaturesBinary *cdatap;
  size_t size;
  if (_user_data_api->lookup_user_data(user_data, cpufeatures_userdata_name, (const void **) &cdatap, &size)) {
    if (size != sizeof(VM_Version::CPUFeaturesBinary)) {
      _user_data_api->destroy_user_data(user_data);
      log_error(crac)("User data %s in %s has unexpected size %zu (expected %zu)", cpufeatures_userdata_name, CRaCRestoreFrom, size, sizeof(VM_Version::CPUFeaturesBinary));
      return false;
    }
    if (cdatap == nullptr) {
      _user_data_api->destroy_user_data(user_data);
      log_error(crac)("lookup_user_data %s should return non-null data pointer", cpufeatures_userdata_name);
      return false;
    }
    *datap = *cdatap;
    *presentp = true;
  } else {
    *presentp = false;
  }
  _user_data_api->destroy_user_data(user_data);
  return true;
}
