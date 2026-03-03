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

#include "crlib/crlib.h"
#include "crlib/crlib_restore_data.h"
#include "logging/log.hpp"
#include "memory/allStatic.hpp"
#include "memory/resourceArea.hpp"
#include "nmt/memTag.hpp"
#include "runtime/crac_engine.hpp"
#include "runtime/globals.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/hashTable.hpp"
#include "utilities/macros.hpp"
#include "utilities/stringUtils.hpp"

#include <cstddef>
#include <cstring>

// CRaC engine configuration options JVM sets directly instead of relaying from the user
#define VM_CONTROLLED_ENGINE_OPTS(OPT) \
  OPT(image_location) \

#define ARRAY_ELEM(opt) #opt,
static constexpr const char * const vm_controlled_engine_opts[] = {
  VM_CONTROLLED_ENGINE_OPTS(ARRAY_ELEM)
};
#undef ARRAY_ELEM

#define DEFINE_OPT_VAR(opt) static constexpr const char engine_opt_##opt[] = #opt;
VM_CONTROLLED_ENGINE_OPTS(DEFINE_OPT_VAR)

DEFINE_OPT_VAR(direct_map)
DEFINE_OPT_VAR(pause)
#undef DEFINE_OPT_VAR

#define SIMENGINE "simengine"

#ifdef LINUX
static bool is_pauseengine() {
  assert(JDK_Version::current().major_version() < 28, "pauseengine shall be expired in JDK 28");
  return !strcmp(CRaCEngine, "pause") || !strcmp(CRaCEngine, "pauseengine");
}
#endif // LINUX

static bool find_engine(const char *dll_dir, char *path, size_t path_size, char *resolved_engine, size_t resolved_engine_size) {
  const size_t engine_length = strlen(CRaCEngine);
  // Try to interpret as a file path
  if (os::is_path_absolute(CRaCEngine)) {
    if (engine_length + 1 > path_size) {
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
    if (strncmp(basename, JNI_LIB_PREFIX, strlen(JNI_LIB_PREFIX)) ||
        strcmp(CRaCEngine + engine_length - strlen(JNI_LIB_SUFFIX), JNI_LIB_SUFFIX)) {
      log_error(crac)("CRaCEngine=%s is not a library: expected " JNI_LIB_PREFIX "<name>" JNI_LIB_SUFFIX, CRaCEngine);
      return false;
    }

    size_t engine_name_length = strlen(basename) - strlen(JNI_LIB_PREFIX) - strlen(JNI_LIB_SUFFIX);
    if (engine_name_length < resolved_engine_size) {
      memcpy(resolved_engine, basename + strlen(JNI_LIB_PREFIX), engine_name_length);
      resolved_engine[engine_name_length] = '\0';
      log_debug(crac)("Found CRaCEngine %s in %s", resolved_engine, CRaCEngine);
      return true;
    } else {
      log_error(crac)("CRaCEngine name is too long: %s", basename);
      return false;
    }
  }

#ifdef LINUX
  if (is_pauseengine()) {
    assert(sizeof(SIMENGINE) <= resolved_engine_size, "must be");
    memcpy(resolved_engine, SIMENGINE, sizeof(SIMENGINE));
    log_warning(crac)("-XX:CRaCEngine=pause/pauseengine is deprecated and will be removed in version 28; use -XX:CRaCEngine=simengine -XX:CRaCEngineOptions=pause=true");
  } else /* intentional line break */
#endif // LINUX
  if (engine_length < resolved_engine_size) {
    memcpy(resolved_engine, CRaCEngine, engine_length);
    resolved_engine[engine_length] = '\0';
  } else {
    log_error(crac)("CRaCEngine name is too long: %s", CRaCEngine);
    return false;
  }

  if (is_vm_statically_linked()) {
    if (!strcmp("sim", resolved_engine)) {
      assert(sizeof(SIMENGINE) <= resolved_engine_size, "must be");
      assert(JDK_Version::current().major_version() < 28, "shall be expired in JDK 28");
      memcpy(resolved_engine, SIMENGINE, sizeof(SIMENGINE));
      log_warning(crac)("Engine name '%s' (without the engine suffix) is deprecated and will be removed in version 28, please use -XX:CRaCEngine=%s", CRaCEngine, resolved_engine);
    }
    log_debug(crac)("VM is statically linked, not doing any library lookup");
    os::jvm_path(path, static_cast<jint>(path_size)); // points to bin/java for static JDK
    return true;
  }

  log_debug(crac)("Resolved engine name '%s'", resolved_engine);
  // Try to interpret as a library name
  if (os::dll_locate_lib(path, path_size, dll_dir, resolved_engine)) {
    log_debug(crac)("Found CRaCEngine %s in %s", resolved_engine, path);
    return true;
  }
  // Alternative for engines without the `engine` suffix.
  // Let's just skip it with weird long engine parameter.
  // Note: resource mark allocation is not possible here (too early in JVM boot)
  if (strlen(CRaCEngine) + strlen("engine") + 1 <= resolved_engine_size) {
    os::snprintf_checked(resolved_engine, resolved_engine_size, "%sengine", CRaCEngine);
    if (os::dll_locate_lib(path, path_size, dll_dir, resolved_engine)) {
      assert(JDK_Version::current().major_version() < 28, "shall be expired in JDK 28");
      log_debug(crac)("Found CRaCEngine %s in %s", resolved_engine, path);
      log_warning(crac)("Engine name '%s' (without the engine suffix) is deprecated and will be removed in version 28, please use -XX:CRaCEngine=%s", CRaCEngine, resolved_engine);
      return true;
    }
  }

  log_error(crac)("Did not find CRaCEngine %s in %s", CRaCEngine, dll_dir);
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
using CStringSet = HashTable<const char *, bool, 256, AnyObj::C_HEAP, MemTag::mtInternal,
                             CStringUtils::hash, CStringUtils::equals>;

static crlib_conf_t *create_conf(const crlib_api_t &api, bool for_restore) {
  crlib_conf_t * const conf = api.create_conf();
  if (conf == nullptr) {
    log_error(crac)("CRaC engine failed to create its configuration");
    return nullptr;
  }

  if (CRaCEngineOptions != nullptr && (!strcmp(CRaCEngineOptions, "help") || !strncmp(CRaCEngineOptions, "help=", 5))) {
    return conf;
  }

#ifdef LINUX
  if (is_pauseengine()) {
    guarantee(api.can_configure(conf, engine_opt_pause), "simengine must support option 'pause'");
    if (!api.configure(conf, engine_opt_pause, "true")) {
      log_error(crac)("simengine failed to configure: '%s' = 'true'", engine_opt_pause);
      api.destroy_conf(conf);
      return nullptr;
    }
  }
#endif // LINUX

  CStringSet keys;
  char* engine_options_start = nullptr;
  if (CRaCEngineOptions != nullptr && CRaCEngineOptions[0] != '\0' /* possible for ccstrlist */) {
    CStringSet vm_controlled_keys;
#define PUT_CONTROLLED_KEY(opt) vm_controlled_keys.put_when_absent(engine_opt_##opt, false);
    VM_CONTROLLED_ENGINE_OPTS(PUT_CONTROLLED_KEY)
#undef PUT_CONTROLLED_KEY

    char *engine_options = os::strdup_check_oom(CRaCEngineOptions, mtInternal);
    engine_options_start = engine_options;
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
  }

  if (for_restore && !keys.contains(engine_opt_direct_map) && api.can_configure(conf, engine_opt_direct_map)) {
    if (!api.configure(conf, engine_opt_direct_map, "true")) {
      log_error(crac)("CRaC engine failed to configure: '%s' = 'true'", engine_opt_direct_map);
      api.destroy_conf(conf);
      return nullptr;
    }
  }

  os::free(engine_options_start);

  return conf;
}

CracEngine::CracEngine(bool for_restore) {
  if (CRaCEngine == nullptr) {
    log_error(crac)("CRaCEngine must not be empty");
    return;
  }

  // Arguments::get_dll_dir() might not have been initialized yet
  char dll_dir[JVM_MAXPATHLEN];
  os::jvm_path(dll_dir, sizeof(dll_dir));
  // path is ".../lib/server/libjvm.so", or "...\bin\server\libjvm.dll"
  // for static JDK, the path or ".../bin/java", or "...\bin\java.exe"
  char *after_elem = nullptr;
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(dll_dir, *os::file_separator());
    *after_elem = '\0';
  }
  if (is_vm_statically_linked()) {
    strcat(dll_dir, os::file_separator());
#ifdef _WINDOWS
    strcat(dll_dir, "bin");
#else
    strcat(dll_dir, "lib");
#endif
  }

  char path[JVM_MAXPATHLEN];
  // In static builds the name of API entry function may differ
  char resolved_engine_func[sizeof(CRLIB_API_FUNC) + MAX_ENGINE_LENGTH];
  memcpy(resolved_engine_func, CRLIB_API_FUNC, sizeof(CRLIB_API_FUNC));
  if (is_vm_statically_linked()) {
    resolved_engine_func[sizeof(CRLIB_API_FUNC) - 1] = '_';
  }

  if (!find_engine(dll_dir, path, sizeof(path), _name, sizeof(_name))) {
    log_error(crac)("Cannot find CRaC engine %s", CRaCEngine);
    return;
  }
  postcond(_name[0] != '\0');
  postcond(path[0] != '\0');
  memcpy(resolved_engine_func + sizeof(CRLIB_API_FUNC), _name, strlen(_name) + 1);

  char error_buf[1024];
  void * const lib = is_vm_statically_linked() ? os::get_default_process_handle() : os::dll_load(path, error_buf, sizeof(error_buf));
  if (lib == nullptr) {
    log_error(crac)("Cannot load CRaC engine library from %s: %s", path, error_buf);
    return;
  }

  using api_func_t = decltype(&CRLIB_API);
  auto api_func = reinterpret_cast<api_func_t>(os::dll_lookup(lib, resolved_engine_func));
  if (api_func == nullptr) {
    if (is_vm_statically_linked()) {
      log_warning(crac)("Cannot load CRaC engine API entrypoint '%s' from %s", resolved_engine_func, path);
      // Maybe the deprecated short name was used, in that case find_engine did not amend the 'engine'
      assert(JDK_Version::current().major_version() < 28, "shall be expired in JDK 28");
      static constexpr const char engine_suffix[] = "engine";
      if (strlen(resolved_engine_func) + sizeof(engine_suffix) <= sizeof(resolved_engine_func)) {
        strcat(resolved_engine_func, engine_suffix);
        assert(strlen(_name) + sizeof(engine_suffix) <= sizeof(_name), "name too long");
        strcat(_name, engine_suffix);
        api_func = reinterpret_cast<api_func_t>(os::dll_lookup(lib, resolved_engine_func));
        if (api_func != nullptr) {
          log_warning(crac)("Engine name '%s' (without the engine suffix) is deprecated and will be removed in version 28, please use -XX:CRaCEngine=%s",
            CRaCEngine, resolved_engine_func + sizeof(CRLIB_API_FUNC));
        }
      }
    }
    if (api_func == nullptr) {
      log_error(crac)("Cannot load CRaC engine API entrypoint '%s' from %s", resolved_engine_func, path);
      os::dll_unload(lib);
      return;
    }
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

  crlib_conf_t * const conf = create_conf(*api, for_restore);
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
  FREE_C_HEAP_ARRAY(crlib_conf_option_t, _options);
}

bool CracEngine::is_initialized() const {
  assert((_lib == nullptr && _api == nullptr && _conf == nullptr) ||
          (_lib != nullptr && _api != nullptr && _conf != nullptr), "invariant");
  return _lib != nullptr;
}

static int open_engine_file(const char *dir, int oflag, const char *purpose) {
#ifndef PATH_MAX
# define PATH_MAX 1024
#endif
  char path[PATH_MAX];
  os::snprintf_checked(path, sizeof(path), "%s/engine", dir);
  int fd = os::open(path, oflag, 0600);
  if (fd < 0) {
    log_error(crac)("Cannot open %s for %s", path, purpose);
  }
  return fd;
}

static bool record_engine(const char *name, const char *dir) {
  assert(dir != nullptr, "Not configured");
  int fd = open_engine_file(dir, O_WRONLY | O_CREAT | O_TRUNC, "writing");
  if (fd < 0 || !os::write(fd, name, strlen(name))) {
    log_error(crac)("Cannot record engine name (%s): %s", name, os::strerror(errno));
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

static bool check_engine(const char *name, const char *dir) {
  int fd = open_engine_file(dir, O_RDONLY, "reading");
  if (fd < 0) {
    return false;
  }
  char buf[MAX_ENGINE_LENGTH];
  size_t rd = 0;
  do {
    ssize_t r = ::read(fd, buf + rd, static_cast<unsigned int>(sizeof(buf) - rd));
    if (r == 0) {
      break;
    } else if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_error(crac)("I/O error reading engine file: %s", os::strerror(errno));
      return false;
    }
    rd += r;
  } while (rd < sizeof(buf));
  ::close(fd);
  buf[rd] = '\0';
  if (strcmp(name, buf)) {
    log_error(crac)("Image format does not match; saved image with engine %s, restoring with %s", buf, name);
    return false;
  }
  return true;
}

int CracEngine::checkpoint() const {
  precond(is_initialized());
  if (!record_engine(_name, _image_location)) {
    return -1;
  }
  return _api->checkpoint(_conf);
}

int CracEngine::restore() const {
  precond(is_initialized());
  if (!check_engine(_name, _image_location)) {
    return -1;
  }
  return _api->restore(_conf);
}

bool CracEngine::configure_image_location(const char *image_location) {
  precond(is_initialized());
  os::free(_image_location);
  _image_location = os::strdup_check_oom(image_location);
  return ::configure_image_location(*_api, _conf, image_location);
}

GrowableArrayCHeap<const char *, MemTag::mtInternal> *CracEngine::vm_controlled_options() const {
  auto * const opts = new GrowableArrayCHeap<const char *, MemTag::mtInternal>();
  // Only list those options which the current engine actually supports
  for (const char *opt : vm_controlled_engine_opts) {
    if (_api->can_configure(_conf, opt)) {
      opts->append(opt);
    }
  }
  return opts;
}

#define prepare_extension_api(_ext_api, ext_name) \
  precond(is_initialized()); \
  if (_ext_api != nullptr) { \
    return ApiStatus::OK; \
  } \
  auto const ext_api = CRLIB_EXTENSION(_api, std::remove_reference<decltype(*_ext_api)>::type, ext_name); \
  if (ext_api == nullptr) { \
    log_debug(crac)("CRaC engine does not support extension " ext_name); \
    return ApiStatus::UNSUPPORTED; \
  } \
  constexpr const char *_ext_name = ext_name;

#define has_method(_ext_api, _func) \
  (_ext_api->header.size >= offsetof(std::remove_reference<decltype(*_ext_api)>::type, _func) + sizeof(_ext_api->_func))

#define require_method(_func) \
  if (has_method(ext_api, _func) && ext_api->_func == nullptr) { \
    log_error(crac)("CRaC engine provided invalid API for extension %s: %s is not set", _ext_name, #_func); \
    return ApiStatus::ERR; \
  }

#define complete_extension_api(_ext_api) \
  _ext_api = ext_api; \
  return ApiStatus::OK;

CracEngine::ApiStatus CracEngine::prepare_restore_data_api() {
  prepare_extension_api(_restore_data_api, CRLIB_EXTENSION_RESTORE_DATA_NAME)
  require_method(set_restore_data)
  require_method(get_restore_data)
  complete_extension_api(_restore_data_api)
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
  prepare_extension_api(_description_api, CRLIB_EXTENSION_DESCRIPTION_NAME)
  require_method(identity)
  require_method(description)
  require_method(configuration_doc)
  require_method(configurable_keys)
  require_method(supported_extensions)
  require_method(configuration_options);
  complete_extension_api(_description_api)
}

const char *CracEngine::description() const {
  precond(_description_api != nullptr);
  return _description_api->description(_conf);
}

const char *CracEngine::configuration_doc() const {
  precond(_description_api != nullptr);
  return _description_api->configuration_doc(_conf);
}

const crlib_conf_option_t *CracEngine::configuration_options() {
  if (_options != nullptr) {
    return _options;
  }
  if (!has_method(_description_api, configuration_options)) {
    return nullptr;
  }
  const crlib_conf_option_t *options = _description_api->configuration_options(_conf);
  if (options == nullptr) {
    return nullptr;
  }
  const crlib_conf_option_t *src = options;
  for (; src->key != nullptr; ++src);
  _options = NEW_C_HEAP_ARRAY(crlib_conf_option_t, src - options + 1, mtInternal);
  crlib_conf_option_t *dst = _options;
  for (src = options; src->key != nullptr; ++src, ++dst) {
    bool skip = false;
    for (const char *opt : vm_controlled_engine_opts) {
      if (!strcmp(src->key, opt)) {
        skip = true;
        break;
      }
    }
    if (skip) {
      --dst;
      continue;
    }
    memcpy(dst, src, sizeof(*src));
    if (!strcmp(dst->key, engine_opt_direct_map)) {
      // JVM is overriding the direct_map default in all engines
      dst->default_value = "true";
    }
  }
  // last element should be zeroes
  memset(dst, 0, sizeof(*dst));
  return _options;
}

static constexpr char cpuarch_name[] = "cpu.arch";
static constexpr char cpufeatures_name[] = "cpu.features";

CracEngine::ApiStatus CracEngine::prepare_image_constraints_api() {
  prepare_extension_api(_image_constraints_api, CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME)
  require_method(set_label)
  require_method(set_bitmap)
  require_method(require_label)
  require_method(require_bitmap)
  require_method(is_failed)
  complete_extension_api(_image_constraints_api)
}

// Return success.
bool CracEngine::store_cpuinfo(const VM_Version::VM_Features *current_features) const {
  log_debug(crac)("store cpu.arch & cpu.features to %s...", CRaCRestoreFrom);
  if (!_image_constraints_api->set_label(_conf, cpuarch_name, ARCHPROPNAME)) {
    log_error(crac)("CRaC engine failed to record label %s", cpuarch_name);
    return false;
  }
  if (!_image_constraints_api->set_bitmap(_conf, cpufeatures_name, reinterpret_cast<const unsigned char *>(current_features), sizeof(*current_features))) {
    log_error(crac)("CRaC engine failed to record bitmap %s", cpufeatures_name);
    return false;
  }
  return true;
}

void CracEngine::require_cpuinfo(const VM_Version::VM_Features *current_features, bool exact) const {
  log_debug(crac)("cpufeatures_load user data %s from %s...", cpufeatures_name, CRaCRestoreFrom);
  _image_constraints_api->require_label(_conf, cpuarch_name, ARCHPROPNAME);
  _image_constraints_api->require_bitmap(_conf, cpufeatures_name,
    reinterpret_cast<const unsigned char *>(current_features), sizeof(*current_features), exact ? EQUALS : SUBSET);
}

void CracEngine::check_cpuinfo(const VM_Version::VM_Features *current_features, bool exact) const {
  if (_image_constraints_api == nullptr) {
    // When CPU features are ignored
    return;
  }
  if (_image_constraints_api->is_failed(_conf, cpuarch_name)) {
    log_error(crac)("Restore failed due to wrong or missing CPU architecture (current architecture is " ARCHPROPNAME ")");
  }
  if (_image_constraints_api->is_failed(_conf, cpufeatures_name)) {
    VM_Version::VM_Features image_features;
    size_t image_features_size = _image_constraints_api->get_failed_bitmap(_conf, cpufeatures_name, reinterpret_cast<unsigned char *>(&image_features), sizeof(image_features));
    if (image_features_size == sizeof(image_features)) {
      ResourceMark rm;
      if (!exact) {
        image_features &= *current_features;
      } else {
        image_features = *current_features;
      }
      log_error(crac)("Restore failed due to incompatible or missing CPU features, try using -XX:CPUFeatures=%s on checkpoint.", image_features.print_numbers());
    } else {
      log_error(crac)("Restore failed due to incompatible or missing CPU features.");
    }
  }
}

bool CracEngine::set_label(const char* label, const char* value) {
  return _image_constraints_api->set_label(_conf, label, value);
}

CracEngine::ApiStatus CracEngine::prepare_image_score_api() {
  prepare_extension_api(_image_score_api, CRLIB_EXTENSION_IMAGE_SCORE_NAME)
  require_method(set_score)
  complete_extension_api(_image_score_api)
}

bool CracEngine::set_score(const char* metric, double value) {
  return _image_score_api->set_score(_conf, metric, value);
}
