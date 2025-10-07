/*
 * Copyright (c) 2023-2025, Azul Systems, Inc. All rights reserved.
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
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "crlib/crlib.h"
#include "crlib/crlib_description.h"
#include "crlib/crlib_image_constraints.h"
#include "crlib/crlib_restore_data.h"
#include "crlib/crlib_user_data.h"
#include "crexec.hpp"
#include "hashtable.hpp"
#include "image_constraints.hpp"
#include "jni.h"

#ifdef LINUX
#include <csignal>

#include "jvm.h"
#endif // LINUX

extern "C" {

JNIEXPORT crlib_api_t *CRLIB_API(int api_version, size_t api_size);

static crlib_conf_t *create_conf();
static void destroy_conf(crlib_conf_t *conf);
static int checkpoint(crlib_conf_t *conf);
static int restore(crlib_conf_t *conf);
static bool can_configure(crlib_conf_t *conf, const char *key);
static bool configure(crlib_conf_t *conf, const char *key, const char *value);
static const crlib_extension_t *get_extension(const char *name, size_t size);

static const char *identity(crlib_conf_t *conf);
static const char *description(crlib_conf_t *conf);
static const char *configuration_doc(crlib_conf_t *conf);
static const char * const *configurable_keys(crlib_conf_t *conf);
static crlib_extension_t * const *supported_extensions(crlib_conf_t *conf);

static bool set_restore_data(crlib_conf_t *conf, const void *data, size_t size);
static size_t get_restore_data(crlib_conf_t *conf, void *buf, size_t size);

static bool set_user_data(crlib_conf_t *conf, const char *name, const void *data, size_t size);
static crlib_user_data_storage_t *load_user_data(crlib_conf_t *conf);
static bool lookup_user_data(crlib_user_data_storage_t *user_data, const char *name, const void **data_p, size_t *size_p);
static void destroy_user_data(crlib_user_data_storage_t *user_data);

static bool set_label(crlib_conf_t *, const char *name, const char *value);
static bool set_bitmap(crlib_conf_t *, const char *name, const unsigned char *value, size_t length_bytes);
static bool require_label(crlib_conf_t *, const char *name, const char *value);
static bool require_bitmap(crlib_conf_t *, const char *name, const unsigned char *value, size_t length_bytes, crlib_bitmap_comparison_t comparison);
static bool is_failed(crlib_conf_t *, const char *name);

} // extern "C"

static crlib_api_t api = {
  create_conf,
  destroy_conf,
  checkpoint,
  restore,
  can_configure,
  configure,
  get_extension,
};

static crlib_description_t description_extension = {
  {
    CRLIB_EXTENSION_DESCRIPTION_NAME,
    sizeof(description_extension)
  },
  identity,
  description,
  configuration_doc,
  configurable_keys,
  supported_extensions,
};

static crlib_restore_data_t restore_data_extension = {
  {
    CRLIB_EXTENSION_RESTORE_DATA_NAME,
    sizeof(restore_data_extension)
  },
  set_restore_data,
  get_restore_data,
};

static crlib_user_data_t user_data_extension = {
  {
    CRLIB_EXTENSION_USER_DATA_NAME,
    sizeof(user_data_extension)
  },
  set_user_data,
  load_user_data,
  lookup_user_data,
  destroy_user_data,
};

static crlib_image_constraints_t image_constraints_extension = {
  {
    CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME,
    sizeof(image_constraints_extension)
  },
  set_label,
  set_bitmap,
  require_label,
  require_bitmap,
  is_failed,
};

static const crlib_extension_t *extensions[] = {
  &restore_data_extension.header,
  &image_constraints_extension.header,
  &user_data_extension.header,
  &description_extension.header,
  nullptr
};


// crexec_md.cpp
const char *file_separator();
bool is_path_absolute(const char *path);
bool exec_child_process_and_wait(const char *path, char * const argv[], char * const env[]);
char **get_environ();
void exec_in_this_process(const char *path, const char *argv[], const char *env[]);

JNIEXPORT crlib_api_t *CRLIB_API(int api_version, size_t api_size) {
  if (api_version != CRLIB_API_VERSION) {
    return nullptr;
  }
  if (sizeof(crlib_api_t) < api_size) {
    return nullptr;
  }
  return &api;
}

// When adding a new option also add its description into the help message,
// ensure the proper default value is set for it in the configuration struct and
// consider checking for inappropriate use on checkpoint/restore.
//
// Place more frequently used options first - this will make them faster to find
// in the options hash table.
#define CONFIGURE_OPTIONS(OPT) \
  OPT(image_location) \
  OPT(exec_location) \
  OPT(keep_running) \
  OPT(direct_map) \
  OPT(args) \

#define DEFINE_OPT(id) static constexpr char opt_##id[] = #id;
CONFIGURE_OPTIONS(DEFINE_OPT)
#undef DEFINE_OPT
#define ADD_ARR_ELEM(id) opt_##id,
static constexpr const char *configure_options[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) nullptr };
#undef ADD_ARR_ELEM

static char *strdup_checked(const char *src) {
  char * const res = strdup(src);
  if (res == nullptr) {
    fprintf(stderr, CREXEC "out of memory\n");
  }
  return res;
}

// Value of a boolean configuration option.
struct BoolOption {
  bool value;
  bool is_default = true;
};

static bool parse_bool(const char *str, BoolOption *result) {
  if (strcmp(str, "true") == 0) {
    *result = {true, false};
    return true;
  }
  if (strcmp(str, "false") == 0) {
    *result = {false, false};
    return true;
  }
  fprintf(stderr, CREXEC "expected '%s' to be either 'true' or 'false'\n", str);
  return false;
}

// Indices of argv array members.
enum Argv : std::uint8_t {
  ARGV_EXEC_LOCATION,
  ARGV_ACTION,
  ARGV_IMAGE_LOCATION,
  ARGV_FREE, // First index for user-provided arguments
  ARGV_LAST = 31,
};

struct crlib_conf {
private:
  using configure_func = bool (crlib_conf::*) (const char *value);
  Hashtable<configure_func> _options {
    configure_options, ARRAY_SIZE(configure_options) - 1 /* omit nullptr */
  };

  BoolOption _keep_running{false};
  BoolOption _direct_map{true};
  int _restore_data = 0;
  const char *_argv[ARGV_LAST + 2] = {}; // Last element is required to be null
  ImageConstraints _image_constraints;

public:
  crlib_conf() {
    if (!_options.is_initialized()) {
      fprintf(stderr, CREXEC "out of memory\n");
      assert(!is_initialized());
      return;
    }
#define PUT_HANDLER(id) _options.put(opt_##id, &crlib_conf::configure_##id);
    CONFIGURE_OPTIONS(PUT_HANDLER)
#undef PUT_HANDLER
  }

  ~crlib_conf() {
    for (int i = 0; i <= ARGV_FREE /* all free args are allocated together */; i++) {
      if (i != ARGV_ACTION) { // Action is a static string
        free(const_cast<char*>(_argv[i]));
      }
    }
  }

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _options.is_initialized(); }

  BoolOption keep_running() const { return _keep_running; }
  BoolOption direct_map() const { return _direct_map; }
  int restore_data() const { return _restore_data; }
  const char * const *argv() const { return _argv; }

  bool can_configure(const char *key) const {
    assert(key != nullptr);
    return _options.contains(key);
  }

  bool configure(const char *key, const char *value) {
    assert(key != nullptr && value != nullptr);
    auto * const func = _options.get(key);
    if (func != nullptr) {
      return (this->**func)(value);
    }
    fprintf(stderr, CREXEC "unknown configure option: %s\n", key);
    return false;
  }

  void set_argv_action(const char *action) { _argv[ARGV_ACTION] = action; }

  bool set_restore_data(const void *data, size_t size) {
    constexpr const size_t supported_size = sizeof(_restore_data);
    if (size > 0 && size != supported_size) {
      fprintf(stderr,
              CREXEC "unsupported size of restore data: %zu was requested but only %zu is supported\n",
              size, supported_size);
      return false;
    }
    if (size > 0) {
      memcpy(&_restore_data, data, size);
    } else {
      _restore_data = 0;
    }
    return true;
  }

  size_t get_restore_data(void *buf, size_t size) {
    constexpr const size_t available_size = sizeof(_restore_data);
    if (size > 0) {
      memcpy(buf, &_restore_data, size < available_size ? size : available_size);
    }
    return available_size;
  }

  ImageConstraints &image_constraints() {
    return _image_constraints;
  }

private:
  bool configure_image_location(const char *image_location) {
    const char *copy = strdup_checked(image_location);
    if (copy == nullptr) {
      return false;
    }
    free(const_cast<char *>(_argv[ARGV_IMAGE_LOCATION]));
    _argv[ARGV_IMAGE_LOCATION] = copy;
    return true;
  }

  bool configure_exec_location(const char *exec_location) {
    if (!is_path_absolute(exec_location)) {
      fprintf(stderr, CREXEC "expected absolute path: %s\n", exec_location);
      return false;
    }
    const char *copy = strdup_checked(exec_location);
    if (copy == nullptr) {
      return false;
    }
    free(const_cast<char *>(_argv[ARGV_EXEC_LOCATION]));
    _argv[ARGV_EXEC_LOCATION] = copy;
    return true;
  }

  bool configure_keep_running(const char *keep_running_str) {
    return parse_bool(keep_running_str, &_keep_running);
  }

  bool configure_direct_map(const char *direct_map_str) {
    return parse_bool(direct_map_str, &_direct_map);
  }

  bool configure_args(const char *args_str) {
    char *arg = strdup_checked(args_str);
    if (arg == nullptr) {
      return false;
    }

    static constexpr int MAX_ARGS_NUM = ARGV_LAST - ARGV_FREE + 1;
    static_assert(MAX_ARGS_NUM >= 0, "sanity check");
    const char *args[MAX_ARGS_NUM];

    int arg_i = 0;
    static constexpr char SEP = ' ';
    for (; arg_i < MAX_ARGS_NUM; arg_i++) {
      args[arg_i] = arg;
      for (; arg[0] != SEP && arg[0] != '\0'; arg++) {}
      if (arg[0] == '\0') {
        break;
      }
      assert(arg[0] == SEP);
      *(arg++) = '\0';
    }

    if (arg[0] != '\0') {
      assert(arg_i == MAX_ARGS_NUM);
      fprintf(stderr, CREXEC "too many free arguments, at most %i allowed\n", MAX_ARGS_NUM);
      free(const_cast<char *>(args[0]));
      return false;
    }

    free(const_cast<char *>(_argv[ARGV_FREE]));
    memcpy(&_argv[ARGV_FREE], args, (arg_i + 1) * sizeof(const char *));
    return true;
  }
};

static crlib_conf_t *create_conf() {
  auto * const conf = new(std::nothrow) crlib_conf();
  if (conf == nullptr || !conf->is_initialized()) {
    delete conf;
    return nullptr;
  }
  return conf;
}

static void destroy_conf(crlib_conf_t *conf) {
  delete conf;
}

static bool can_configure(crlib_conf_t *conf, const char *key) {
  return conf->can_configure(key);
}

static bool configure(crlib_conf_t *conf, const char *key, const char *value) {
  return conf->configure(key, value);
}

static const char *identity(crlib_conf_t *conf) {
  return "crexec";
}

static const char *description(crlib_conf_t *conf) {
  return
    "crexec - pseudo-CRaC-engine used to relay data from JVM to a \"real\" engine implemented as "
    "an executable (instead of a library). The engine executable is expected to have "
    "CRaC-CRIU-like CLI. Support of the configuration options also depends on the engine "
    "executable.";
}

static const char *configuration_doc(crlib_conf_t *conf) {
  return
    "* image_location=<path> (no default) - path to a directory with checkpoint/restore files.\n"
    "* exec_location=<path> (no default) - path to the engine executable.\n"
    "* keep_running=<true/false> (default: false) - keep the process running after the checkpoint "
    "or kill it.\n"
    "* direct_map=<true/false> (default: true) - on restore, map process data directly from saved "
    "files. This may speedup the restore but the resulting process will not be the same as before "
    "the checkpoint.\n"
    "* args=<string> (default: \"\") - free space-separated arguments passed directly to the "
    "engine executable, e.g. \"--arg1 --arg2 --arg3\".\n";
}

static const char * const *configurable_keys(crlib_conf_t *conf) {
  return configure_options;
}

static crlib_extension_t * const *supported_extensions(crlib_conf_t *conf) {
  return extensions;
}

static bool set_restore_data(crlib_conf_t *conf, const void *data, size_t size) {
  return conf->set_restore_data(data, size);
}

static size_t get_restore_data(crlib_conf_t *conf, void *buf, size_t size) {
  return conf->get_restore_data(buf, size);
}

static bool set_user_data(crlib_conf_t *conf, const char *name, const void *data, size_t size) {
  if (!conf->argv()[ARGV_IMAGE_LOCATION]) {
    fprintf(stderr, CREXEC "configure_image_location has not been called\n");
    return false;
  }
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/%s", conf->argv()[ARGV_IMAGE_LOCATION], name) >= (int) sizeof(fname) - 1) {
    fprintf(stderr, CREXEC "filename too long: %s/%s\n", conf->argv()[ARGV_IMAGE_LOCATION], name);
    return false;
  }
  FILE *f = fopen(fname, "w");
  if (f == nullptr) {
    fprintf(stderr, CREXEC "cannot create %s: %s\n", fname, strerror(errno));
    return false;
  }
  while (size--) {
    uint8_t byte = *(const uint8_t *) data;
    data = (const void *) ((uintptr_t) data + 1);
    if (fprintf(f, "%02x", byte) != 2) {
      fclose(f);
      fprintf(stderr, CREXEC "cannot write to %s: %s\n", fname, strerror(errno));
      return false;
    }
  }
  if (fputc('\n', f) != '\n') {
    fclose(f);
    fprintf(stderr, CREXEC "cannot write to %s: %s\n", fname, strerror(errno));
    return false;
  }
  if (fclose(f)) {
    fprintf(stderr, CREXEC "cannot close %s: %s\n", fname, strerror(errno));
    return false;
  }
  return true;
}

struct user_data_chunk {
  struct user_data_chunk *next;
  size_t size;
  uint8_t *data;
};

struct crlib_user_data_storage {
  crlib_conf_t *conf;
  struct user_data_chunk *chunk;
};

static crlib_user_data_storage_t *load_user_data(crlib_conf_t *conf) {
  crlib_user_data_storage_t *user_data = static_cast<crlib_user_data_storage_t *>(malloc(sizeof(*user_data)));
  if (user_data == nullptr) {
    fprintf(stderr, CREXEC "cannot allocate memory\n");
    return nullptr;
  }
  user_data->conf = conf;
  user_data->chunk = nullptr;
  return user_data;
}

static bool lookup_user_data(crlib_user_data_storage_t *user_data, const char *name, const void **data_p, size_t *size_p) {
  const crlib_conf_t *conf = user_data->conf;
  if (!conf->argv()[ARGV_IMAGE_LOCATION]) {
    fprintf(stderr, CREXEC "configure_image_location has not been called\n");
    return false;
  }
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/%s", conf->argv()[ARGV_IMAGE_LOCATION], name) >= (int) sizeof(fname) - 1) {
    fprintf(stderr, CREXEC "filename is too long: %s/%s\n", conf->argv()[ARGV_IMAGE_LOCATION], name);
    return false;
  }
  FILE *f = fopen(fname, "r");
  if (f == nullptr) {
    if (errno != ENOENT) {
      fprintf(stderr, CREXEC "cannot open %s: %s\n", fname, strerror(errno));
    }
    return false;
  }
  uint8_t *data = nullptr;
  size_t data_used = 0;
  size_t data_allocated = 0;
  int nibble = -1;
  for (;;) {
    int gotc = fgetc(f);
    if (gotc == EOF) {
      fclose(f);
      free(data);
      fprintf(stderr, CREXEC "unexpected EOF or error in %s after %zu parsed bytes\n", fname, data_used);
      return false;
    }
    if (gotc == '\n' && nibble == -1) {
      break;
    }
    if (gotc >= '0' && gotc <= '9') {
      gotc += -'0';
    } else if (gotc >= 'a' && gotc <= 'f') {
      gotc += -'a' + 0xa;
    } else {
      fclose(f);
      free(data);
      fprintf(stderr, CREXEC "unexpected character 0x%02x in %s after %zu parsed bytes\n", gotc, fname, data_used);
      return false;
    }
    if (nibble == -1) {
      nibble = gotc;
      continue;
    }
    if (data_used == data_allocated) {
      data_allocated *= 2;
      if (!data_allocated) {
        data_allocated = 0x100;
      }
      uint8_t *data_new = static_cast<uint8_t *>(realloc(data, data_allocated));
      if (data_new == nullptr) {
        fclose(f);
        free(data);
        fprintf(stderr, CREXEC "cannot allocate memory for %s after %zu parsed bytes\n", fname, data_used);
        return false;
      }
      data = data_new;
    }
    assert(data_used < data_allocated);
    data[data_used++] = (nibble << 4) | gotc;
    nibble = -1;
  }
  if (fgetc(f) != EOF || !feof(f) || ferror(f)) {
    fclose(f);
    free(data);
    fprintf(stderr, CREXEC "EOF expected after newline in %s after %zu parsed bytes\n", fname, data_used);
    return false;
  }
  if (fclose(f)) {
    free(data);
    fprintf(stderr, CREXEC "error closing %s after %zu parsed bytes\n", fname, data_used);
    return false;
  }
  *data_p = data;
  *size_p = data_used;
  struct user_data_chunk *chunk = static_cast<struct user_data_chunk *>(malloc(sizeof(*chunk)));
  if (chunk == nullptr) {
    free(data);
    fprintf(stderr, CREXEC "cannot allocate memory\n");
    return false;
  }
  chunk->next = user_data->chunk;
  user_data->chunk = chunk;
  chunk->size = data_used;
  chunk->data = data;
  return true;
}

static void destroy_user_data(crlib_user_data_storage_t *user_data) {
  while (user_data->chunk) {
    struct user_data_chunk *chunk = user_data->chunk;
    user_data->chunk = chunk->next;
    free(chunk->data);
    free(chunk);
  }
  free(user_data);
}

static bool set_label(crlib_conf_t *conf, const char *name, const char *value) {
  return conf->image_constraints().set_label(name, value);
}

static bool set_bitmap(crlib_conf_t *conf, const char *name, const unsigned char *value, size_t length_bytes) {
  return conf->image_constraints().set_bitmap(name, value, length_bytes);
}

static bool require_label(crlib_conf_t *conf, const char *name, const char *value) {
  return conf->image_constraints().require_label(name, value);
}

static bool require_bitmap(crlib_conf_t *conf, const char *name, const unsigned char *value, size_t length_bytes, crlib_bitmap_comparison_t comparison) {
  return conf->image_constraints().require_bitmap(name, value, length_bytes, comparison);
}

static bool is_failed(crlib_conf_t *conf, const char *name) {
  return conf->image_constraints().is_failed(name);
}

static const crlib_extension_t *get_extension(const char *name, size_t size) {
  for (size_t i = 0; i < ARRAY_SIZE(extensions) - 1 /* omit nullptr */; i++) {
    const crlib_extension_t *ext = extensions[i];
    if (strcmp(name, ext->name) == 0) {
      if (size <= ext->size) {
        return ext;
      }
      return nullptr;
    }
  }
  return nullptr;
}

class Environment {
private:
  char **_env;
  size_t _length;

public:
  explicit Environment(const char * const *env = get_environ()) {
    _length = 0;
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

  ~Environment() {
    if (is_initialized()) {
      for (size_t i = 0; i < _length; i++) {
        free(_env[i]);
      }
      free(_env);
    }
  }

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _env != nullptr; }

  char **env() { return _env; }

  bool append(const char *var, const char *value) {
    assert(is_initialized());

    const size_t str_size = strlen(var) + strlen("=") + strlen(value) + 1;
    char * const str = static_cast<char *>(malloc(sizeof(char) * str_size));
    if (str == nullptr) {
      fprintf(stderr, CREXEC "out of memory\n");
      return false;
    }
    if (snprintf(str, str_size, "%s=%s", var, value) != static_cast<int>(str_size) - 1) {
      perror(CREXEC "snprintf env var");
      free(str);
      return false;
    }

    {
      char ** const new_env = static_cast<char **>(realloc(_env, (_length + 2) * sizeof(char *)));
      if (new_env == nullptr) {
        fprintf(stderr, CREXEC "out of memory\n");
        free(str);
        return false;
      }
      _env = new_env;
    }

    _env[_length++] = str;
    _env[_length] = nullptr;

    return true;
  }

  bool add_criu_option(const char *opt) {
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
      fprintf(stderr, CREXEC "out of memory\n");
      return false;
    }
    if (snprintf(new_opts, new_opts_size, "%s %s", _env[opts_index], opt) !=
          static_cast<int>(new_opts_size) - 1) {
      perror(CREXEC "snprintf CRAC_CRIU_OPTS (append)");
      free(new_opts);
      return false;
    }
    free(_env[opts_index]);
    _env[opts_index] = new_opts;

    return true;
  }
};

static int checkpoint(crlib_conf_t *conf) {
  if (conf->argv()[ARGV_EXEC_LOCATION] == nullptr) {
    fprintf(stderr, CREXEC "%s must be set before checkpoint\n", opt_exec_location);
    return -1;
  }
  if (conf->argv()[ARGV_IMAGE_LOCATION] == nullptr) {
    fprintf(stderr, CREXEC "%s must be set before checkpoint\n", opt_image_location);
    return -1;
  }
  conf->set_argv_action("checkpoint");

  if (!conf->direct_map().is_default) {
    fprintf(stderr, CREXEC "%s has no effect on checkpoint\n", opt_direct_map);
  }

  if (!conf->image_constraints().persist(conf->argv()[ARGV_IMAGE_LOCATION])) {
    return -1;
  }

  {
    Environment env;
    if (!env.is_initialized() ||
        (conf->keep_running().value && !env.append("CRAC_CRIU_LEAVE_RUNNING", ""))) {
      return -1;
    }

    if (!exec_child_process_and_wait(conf->argv()[ARGV_EXEC_LOCATION],
                                     const_cast<char **>(conf->argv()), env.env())) {
      return -1;
    }
  }

#ifdef LINUX
  siginfo_t info;
  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, RESTORE_SIGNAL);

  int sig;
  do {
    sig = sigwaitinfo(&waitmask, &info);
  } while (sig == -1 && errno == EINTR);

  if (info.si_code != SI_QUEUE) {
    return false;
  }
  {
#ifndef NDEBUG
    const bool ok =
#endif // NDEBUG
    conf->set_restore_data(&info.si_int, sizeof(info.si_int));
    assert(ok);
  }
#endif // LINUX

  return 0;
}

static int restore(crlib_conf_t *conf) {
  if (conf->argv()[ARGV_EXEC_LOCATION] == nullptr) {
    fprintf(stderr, CREXEC "%s must be set before restore\n", opt_exec_location);
    return -1;
  }
  if (conf->argv()[ARGV_IMAGE_LOCATION] == nullptr) {
    fprintf(stderr, CREXEC "%s must be set before restore\n", opt_image_location);
    return -1;
  }
  conf->set_argv_action("restore");

  if (!conf->keep_running().is_default) {
    fprintf(stderr, CREXEC "%s has no effect on restore\n", opt_keep_running);
  }

  if (!conf->image_constraints().validate(conf->argv()[ARGV_IMAGE_LOCATION])) {
    return -1;
  }

  char restore_data_str[32];
  if (snprintf(restore_data_str, sizeof(restore_data_str), "%i", conf->restore_data()) >
      static_cast<int>(sizeof(restore_data_str)) - 1) {
    perror(CREXEC "snprintf restore data");
    return -1;
  }

  Environment env;
  if (!env.is_initialized() ||
      !env.append("CRAC_NEW_ARGS_ID", restore_data_str) ||
      (!conf->direct_map().value && !env.add_criu_option("--no-mmap-page-image"))) {
    return -1;
  }

  exec_in_this_process(conf->argv()[ARGV_EXEC_LOCATION],
                       const_cast<const char **>(conf->argv()),
                       const_cast<const char **>(env.env()));

  fprintf(stderr, CREXEC "restore failed\n");
  return -1;
}
