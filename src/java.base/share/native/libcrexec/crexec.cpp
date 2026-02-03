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

#include "crlib/crlib_description.h"
#include "crlib/crlib_restore_data.h"
#include "crlib/crlib_user_data.h"
#include "crcommon.hpp"
#include "environment.hpp"
#include "hashtable.hpp"
#include "image_constraints.hpp"
#include "image_score.hpp"
#include "user_data.hpp"
#include "jni.h"

#ifdef LINUX
#include <csignal>

#include "jvm.h"
#endif // LINUX

// crexec_md.cpp
const char *file_separator();
bool is_path_absolute(const char *path);
bool exec_child_process_and_wait(const char *path, char * const argv[], char * const env[]);
void exec_in_this_process(const char *path, const char *argv[], const char *env[]);

// When adding a new option ensure the proper default value is set for it
// in the configuration struct.
//
// Place more frequently used options first - this will make them faster to find
// in the options hash table.
#define UNCHECKED_OPTIONS(OPT) \
  OPT(image_location, const char *, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "path", "no default", \
    "path to a directory with checkpoint/restore files.") \
  OPT(exec_location, const char *, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "path", "no default", \
    "path to the engine executable.") \
  OPT(args, const char *, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "string", "\"\"", \
    "free space-separated arguments passed directly to the engine executable, e.g. \"--arg1 --arg2 --arg3\".") \

#define CHECKED_OPTIONS(OPT) \
  OPT(keep_running, bool, false, CRLIB_OPTION_FLAG_CHECKPOINT, "true/false", "false", \
    "keep the process running after the checkpoint or kill it.") \
  OPT(direct_map, bool, true, CRLIB_OPTION_FLAG_RESTORE, "true/false", "true", \
    "on restore, map process data directly from saved files. This may speedup the restore " \
    "but the resulting process will not be the same as before the checkpoint.") \

#define CONFIGURE_OPTIONS(OPT) UNCHECKED_OPTIONS(OPT) CHECKED_OPTIONS(OPT)

#define DEFINE_OPT(id, ...) static constexpr char opt_##id[] = #id;
CONFIGURE_OPTIONS(DEFINE_OPT)
#undef DEFINE_OPT
#define ADD_ARR_ELEM(id, ...) opt_##id,
static constexpr const char *configure_options_names[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) nullptr };
#undef ADD_ARR_ELEM
#define ADD_ARR_ELEM(id, ctype, cdef, flags, ...) { opt_##id, static_cast<crlib_conf_option_flag_t>(flags), __VA_ARGS__ },
static constexpr const crlib_conf_option_t configure_options[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) {} };
#undef ADD_ARR_ELEM

static char *strdup_checked(const char *src) {
  char * const res = strdup(src);
  if (res == nullptr) {
    LOG("out of memory");
  }
  return res;
}

template <typename T> struct Option {
  T value;
  bool is_default = true;
};

static bool parse_bool(const char *str, Option<bool> *result) {
  if (strcmp(str, "true") == 0) {
    *result = {true, false};
    return true;
  }
  if (strcmp(str, "false") == 0) {
    *result = {false, false};
    return true;
  }
  LOG("expected '%s' to be either 'true' or 'false'", str);
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

class CrExec: public crlib_conf_t {
private:
  using configure_func = bool (CrExec::*) (const char *value);
  Hashtable<configure_func> _options {
    configure_options_names, ARRAY_SIZE(configure_options_names) - 1 /* omit nullptr */
  };

#define DEFINE_DEFAULT(id, ctype, cdef, ...) \
  private: Option<ctype> _##id = { cdef, true }; \
  public: inline ctype id() const { return _##id.value; }
  CHECKED_OPTIONS(DEFINE_DEFAULT)
#undef DEFINE_DEFAULT

private:
  int _restore_data = 0;
  const char *_argv[ARGV_LAST + 2] = {}; // Last element is required to be null

  UserData _user_data;

public:
  CrExec(): _user_data(&_argv[ARGV_IMAGE_LOCATION]) {
    if (!init_conf(this, "crexec")) {
      return;
    }
    if (!_options.is_initialized()) {
      LOG("out of memory");
      assert(!is_initialized());
      return;
    }
#define PUT_HANDLER(id, ...) _options.put(opt_##id, &CrExec::configure_##id);
    CONFIGURE_OPTIONS(PUT_HANDLER)
#undef PUT_HANDLER
  }

  ~CrExec() {
    for (int i = 0; i <= ARGV_FREE /* all free args are allocated together */; i++) {
      if (i != ARGV_ACTION) { // Action is a static string
        free(const_cast<char*>(_argv[i]));
      }
    }
    destroy_conf(this);
  }

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _options.is_initialized(); }

  int restore_data() const { return _restore_data; }
  const char * const *argv() const { return _argv; }

  void require_defaults(crlib_conf_option_flag_t flag, const char *event) const {
#define CHECK_OPT(id, ctype, cdef, flags, ...) \
  if (!_##id.is_default && !((flags) & flag)) { \
    LOG(#id " has no effect on %s", event); \
  }
    CHECKED_OPTIONS(CHECK_OPT)
#undef CHECK_OPT
  }

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
    LOG("unknown configure option: %s", key);
    return false;
  }

  void set_argv_action(const char *action) { _argv[ARGV_ACTION] = action; }

  bool set_restore_data(const void *data, size_t size) {
    constexpr const size_t supported_size = sizeof(_restore_data);
    if (size > 0 && size != supported_size) {
      LOG("unsupported size of restore data: %zu was requested but only %zu is supported", size, supported_size);
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

  UserData &user_data() {
    return _user_data;
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
      LOG("expected absolute path: %s", exec_location);
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
      LOG("too many free arguments, at most %i allowed", MAX_ARGS_NUM);
      free(const_cast<char *>(args[0]));
      return false;
    }

    free(const_cast<char *>(_argv[ARGV_FREE]));
    memcpy(&_argv[ARGV_FREE], args, (arg_i + 1) * sizeof(const char *));
    return true;
  }
};

static crlib_conf_t *create_crexec() {
  auto * const conf = new(std::nothrow) CrExec();
  if (conf == nullptr || !conf->is_initialized()) {
    delete conf;
    return nullptr;
  }
  return conf;
}

static void destroy_crexec(crlib_conf_t *conf) {
  delete static_cast<CrExec*>(conf);
}

static bool can_configure(crlib_conf_t *conf, const char *key) {
  return static_cast<CrExec*>(conf)->can_configure(key);
}

static bool configure(crlib_conf_t *conf, const char *key, const char *value) {
  return static_cast<CrExec*>(conf)->configure(key, value);
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
#define DOC_ITEM(name, ctype, cdef, flags, type, _default, description) \
  "* " #name "=<" type "> (default: " _default ") - " description "\n"
  return CONFIGURE_OPTIONS(DOC_ITEM);
#undef DOC_ITEM
}

static const char * const *configurable_keys(crlib_conf_t *conf) {
  return configure_options_names;
}

static const crlib_conf_option_t *configuration_options(crlib_conf_t *conf) {
   return configure_options;
}

static bool set_restore_data(crlib_conf_t *conf, const void *data, size_t size) {
  return static_cast<CrExec*>(conf)->set_restore_data(data, size);
}

static size_t get_restore_data(crlib_conf_t *conf, void *buf, size_t size) {
  return static_cast<CrExec*>(conf)->get_restore_data(buf, size);
}

static bool set_user_data(crlib_conf_t *conf, const char *name, const void *data, size_t size) {
  return static_cast<CrExec*>(conf)->user_data().set_user_data(name, data, size);
}

static crlib_user_data_storage_t *load_user_data(crlib_conf_t *conf) {
  return static_cast<CrExec*>(conf)->user_data().load_user_data();
}

static bool lookup_user_data(crlib_user_data_storage_t *storage, const char *name, const void **data_p, size_t *size_p) {
  return storage->user_data->lookup_user_data(storage, name, data_p, size_p);
}

static void destroy_user_data(crlib_user_data_storage_t *storage) {
  return storage->user_data->destroy_user_data(storage);
}

static int checkpoint(crlib_conf_t *c) {
  CrExec *conf = static_cast<CrExec *>(c);
  if (conf->argv()[ARGV_EXEC_LOCATION] == nullptr) {
    LOG("%s must be set before checkpoint", opt_exec_location);
    return -1;
  }
  const char *image_location = conf->argv()[ARGV_IMAGE_LOCATION];
  if (image_location == nullptr) {
    LOG("%s must be set before checkpoint", opt_image_location);
    return -1;
  }
  conf->set_argv_action("checkpoint");
  conf->require_defaults(CRLIB_OPTION_FLAG_CHECKPOINT, "checkpoint");

  if (!image_constraints_persist(conf, image_location) ||
      !image_score_persist(conf, image_location)) {
    return -1;
  }
  // We will reset scores now; scores can be retained or reset higher on the Java level.
  // Before another checkpoint all the scores will be recorded again; we won't keep
  // anything here to not write down any outdated value.
  image_score_reset(conf);

  {
    Environment env;
    if (!env.is_initialized() ||
        (conf->keep_running() && !env.append("CRAC_CRIU_LEAVE_RUNNING", ""))) {
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

static int restore(crlib_conf_t *c) {
  CrExec *conf = static_cast<CrExec *>(c);
  if (conf->argv()[ARGV_EXEC_LOCATION] == nullptr) {
    LOG("%s must be set before restore", opt_exec_location);
    return -1;
  }
  if (conf->argv()[ARGV_IMAGE_LOCATION] == nullptr) {
    LOG("%s must be set before restore", opt_image_location);
    return -1;
  }
  conf->set_argv_action("restore");
  conf->require_defaults(CRLIB_OPTION_FLAG_RESTORE, "restore");

  if (!image_constraints_validate(conf, conf->argv()[ARGV_IMAGE_LOCATION])) {
    return -1;
  }

  char restore_data_str[32];
  if (snprintf(restore_data_str, sizeof(restore_data_str), "%i", conf->restore_data()) >
      static_cast<int>(sizeof(restore_data_str)) - 1) {
    LOG("snprintf restore data: %s", strerror(errno));
    return -1;
  }

  Environment env;
  if (!env.is_initialized() ||
      !env.append("CRAC_NEW_ARGS_ID", restore_data_str) ||
      (!conf->direct_map() && !env.add_criu_option("--no-mmap-page-image"))) {
    return -1;
  }

  exec_in_this_process(conf->argv()[ARGV_EXEC_LOCATION],
                       const_cast<const char **>(conf->argv()),
                       const_cast<const char **>(env.env()));

  LOG("restore failed");
  return -1;
}

static crlib_extension_t * const *supported_extensions(crlib_conf_t *conf);

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
  configuration_options,
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

static const crlib_extension_t *extensions[] = {
  &restore_data_extension.header,
  &image_constraints_extension.header,
  &image_score_extension.header,
  &user_data_extension.header,
  &description_extension.header,
  nullptr
};

static const crlib_extension_t *get_extension(const char *name, size_t size) {
  return find_extension(extensions, name, size);
}

static crlib_extension_t * const *supported_extensions(crlib_conf_t *conf) {
  return extensions;
}

static crlib_api_t api = {
  create_crexec,
  destroy_crexec,
  checkpoint,
  restore,
  can_configure,
  configure,
  get_extension,
};

extern "C" {

JNIEXPORT crlib_api_t *CRLIB_API_MAYBE_STATIC(int api_version, size_t api_size) {
  if (api_version != CRLIB_API_VERSION) {
    return nullptr;
  }
  if (sizeof(crlib_api_t) < api_size) {
    return nullptr;
  }
  return &api;
}

}
