#include "precompiled.hpp"

#include "crlib/crlib.h"
#include "crlib/crlib_restore_data.h"
#include "logging/log.hpp"
#include "runtime/crac_engine.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"

#include <cstddef>
#include <cstring>

// CRaC engine configuration options JVM sets directly instead of relaying from the user
#define ENGINE_OPT_IMAGE_LOCATION "image_location"
#define ENGINE_OPT_EXEC_LOCATION "exec_location"

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

#ifdef _WINDOWs
  const char *suffix = ".exe";
#else
  const char *suffix = "";
#endif // ! _WINDOWS
#ifndef S_ISREG
# define S_ISREG(__mode) ((__mode & S_IFMT) == S_IFREG)
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
  if (!api.configure(conf, ENGINE_OPT_IMAGE_LOCATION, image_location)) {
    log_error(crac)("CRaC engine failed to configure: '" ENGINE_OPT_IMAGE_LOCATION "' = '%s'", image_location);
    return false;
  }
  return true;
}

static crlib_conf_t *create_conf(const crlib_api_t &api, const char *image_location, const char *exec_location) {
  crlib_conf_t * const conf = api.create_conf();
  if (conf == nullptr) {
    log_error(crac)("CRaC engine failed to create its configuration");
    return nullptr;
  }

  if (!configure_image_location(api, conf, image_location)) {
    api.destroy_conf(conf);
    return nullptr;
  }

  if (exec_location != nullptr) { // Only passed when using crexec
    guarantee(api.can_configure(conf, ENGINE_OPT_EXEC_LOCATION),
              "crexec does not support an internal option: " ENGINE_OPT_EXEC_LOCATION);
    if (!api.configure(conf, ENGINE_OPT_EXEC_LOCATION, exec_location)) {
      log_error(crac)("crexec failed to configure: '" ENGINE_OPT_EXEC_LOCATION "' = '%s'", exec_location);
      api.destroy_conf(conf);
      return nullptr;
    }
  }

  if (CRaCEngineOptions == nullptr || CRaCEngineOptions[0] == '\0' /* possible for ccstrlist */) {
    return conf;
  }

  char *engine_options = os::strdup_check_oom(CRaCEngineOptions, mtInternal);
  char *const engine_options_start = engine_options;
  do {
    char *key_value = strsep(&engine_options, ",\n"); // '\n' appears when ccstrlist is appended to
    const char *key = strsep(&key_value, "=");
    const char *value = key_value != nullptr ? key_value : "";
    assert(key != nullptr, "Should have terminated before");
    if (strcmp(key, ENGINE_OPT_IMAGE_LOCATION) == 0 ||
        (exec_location != nullptr && strcmp(key, ENGINE_OPT_EXEC_LOCATION) == 0)) {
      log_warning(crac)("Internal CRaC engine option provided, skipping: %s", key);
      continue;
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
    log_error(crac)("CRaC engine provided invalid restore data API");
    return ApiStatus::ERROR;
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
