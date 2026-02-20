/*
 * Copyright (c) 2021, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef LINUX
#include <signal.h>
#include <unistd.h>
#endif // LINUX

#include "crcommon.hpp"
#include "crlib/crlib_restore_data.h"
#include "crlib/crlib_description.h"
#include "jni.h"

#ifdef LINUX
# define LINUX_ONLY(x) x

int kickjvm(pid_t jvm, int code);
int waitjvm();

#else
# define LINUX_ONLY(x)
typedef int pid_t;
#endif // !LINUX

#define CONFIGURE_OPTIONS(OPT) \
  OPT(image_location, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "path", "no default", \
    "path to a directory with checkpoint/restore files.") \
  LINUX_ONLY(OPT(pause, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "true/false", "false", \
    "on checkpoint don't continue immediately; on restore wake up the waiting process"))

#define DEFINE_NAME(id, ...) constexpr const char* const opt_##id = #id;
CONFIGURE_OPTIONS(DEFINE_NAME);
#undef DEFINE_NAME

class simengine: public crlib_base_t {
public:
  char* image_location = nullptr;
  bool pause = false;

  bool has_restore_data = false;
  int restore_data = 0;

  simengine():
    crlib_base_t("simengine") {
  }
  ~simengine() {
    free(image_location);
  }
};

RENAME_CRLIB(simengine);

static crlib_conf_t* create_simengine() {
    simengine* conf = new(std::nothrow) simengine();
    if (conf == nullptr) {
      LOG("Cannot create simengine instance (out of memory)");
      return nullptr;
    } else if (conf->common() == nullptr) {
      delete conf;
      return nullptr;
    }
    return static_cast<crlib_conf_t*>(conf);
}

static void destroy_simengine(crlib_conf_t* conf) {
  delete static_cast<simengine*>(conf);
}

static int checkpoint(crlib_conf_t* conf) {
  if (!image_constraints_persist(conf->common(), conf->image_location) ||
      !image_score_persist(conf->common(), conf->image_location)) {
    return -1;
  }
  image_score_reset(conf->common());

#ifdef LINUX
  if (!conf->pause) {
    // Return immediately
    return 0;
  }

  char pidpath[1024];
  if ((size_t) snprintf(pidpath, sizeof(pidpath), "%s/pid", conf->image_location) >= sizeof(pidpath)) {
    return 1;
  }
  pid_t jvm = getpid();

  FILE *pidfile = fopen(pidpath, "w");
  if (!pidfile) {
    LOG("fopen pidfile: %s", strerror(errno));
    return 1;
  }

  fprintf(pidfile, "%d\n", jvm);
  fclose(pidfile);

  LOG("pausing the process, restore from another process to unpause it");
  conf->restore_data = waitjvm();
#else // !LINUX
  assert(!conf->pause);
#endif // !LINUX
  return 0;
}

static int restore(crlib_conf_t* conf) {
  if (!image_constraints_validate(conf->common(), conf->image_location)) {
    return -1;
  }

#ifdef LINUX
  if (!conf->pause) {
    LOG("restore requires -XX:CRaCEngineOptions=pause=true to wake the process paused with this option.");
    return -1;
  }
  char pidpath[1024];
  if ((size_t) snprintf(pidpath, sizeof(pidpath), "%s/pid", conf->image_location) >= sizeof(pidpath)) {
    return -1;
  }
  FILE *pidfile = fopen(pidpath, "r");
  if (!pidfile) {
    LOG("fopen pidfile: %s", strerror(errno));
    return -1;
  }

  pid_t jvm;
  if (1 != fscanf(pidfile, "%d", &jvm)) {
    LOG("fscanf pidfile: %s", strerror(errno));
    fclose(pidfile);
    return -1;
  }
  fclose(pidfile);

  if (kickjvm(jvm, conf->restore_data)) {
    LOG("error unpausing checkpointed process (pid %d)", jvm);
  } else {
    LOG("successfully unpaused the checkpointed process\n");
  }

  // Do not return; terminate the restoring JVM immediatelly
  exit(0);
#else // if !LINUX
  assert(!conf->pause);
  LOG("restore is not supported as a separate action by this engine, "
    "it always restores a process immediately after checkpointing it");
  return -1;
#endif // !LINUX
}

static bool can_configure(crlib_conf_t* conf, const char* key) {
  return !strcmp(key, opt_image_location) LINUX_ONLY(|| !strcmp(key, opt_pause));
}

static bool configure(crlib_conf_t* conf, const char* key, const char* value) {
  if (!strcmp(key, opt_image_location)) {
    char* copy = strdup(value);
    if (value == nullptr) {
      LOG("out of memory");
      return false;
    }
    free(conf->image_location);
    conf->image_location = copy;
    return true;
#ifdef LINUX
  } else if (!strcmp(key, opt_pause)) {
    if (!strcasecmp(value, "true")) {
      conf->pause = true;
    } else if (!strcasecmp(value, "false")) {
      conf->pause = false;
    } else {
      LOG("expected %s to be either 'true' or 'false'", key);
      return false;
    }
    return true;
#endif // LINUX
  }
  LOG("unknown configure option: %s", key);
  return false;
}

static bool set_restore_data(crlib_conf_t *conf, const void *data, size_t size) {
  constexpr const size_t supported_size = sizeof(conf->restore_data);
  if (size > 0 && size != supported_size) {
    LOG("unsupported size of restore data: %zu was requested but only %zu is supported", size, supported_size);
    return false;
  }
  if (size > 0) {
    memcpy(&conf->restore_data, data, size);
    conf->has_restore_data = true;
  } else {
    conf->restore_data = 0;
    conf->has_restore_data = false;
  }
  return true;
}

static size_t get_restore_data(crlib_conf_t *conf, void *buf, size_t size) {
  if (!conf->has_restore_data) {
    return 0;
  }
  constexpr const size_t available_size = sizeof(conf->restore_data);
  if (size > 0) {
    memcpy(buf, &conf->restore_data, size < available_size ? size : available_size);
  }
  return available_size;
}

static crlib_restore_data_t restore_data_extension = {
  {
    CRLIB_EXTENSION_RESTORE_DATA_NAME,
    sizeof(restore_data_extension)
  },
  set_restore_data,
  get_restore_data,
};

static const char *identity(crlib_conf_t *conf) {
  return "simengine";
}

static const char *description(crlib_conf_t *conf) {
  return
    "simengine - CRaC-engine used for development & testing; does not implement "
    "actual process checkpoint & restoration but only simulates these.";
}

static const char *configuration_doc(crlib_conf_t *conf) {
#define DOC_ITEM(name, flags, type, _default, description) \
  "* " #name "=<" type "> (default: " _default ") - " description "\n"
  return CONFIGURE_OPTIONS(DOC_ITEM);
#undef DOC_ITEM
}

static const char * const *configurable_keys(crlib_conf_t *conf) {
#define GET_ID(id, ...) opt_##id,
  static constexpr const char *configure_options_names[] = { CONFIGURE_OPTIONS(GET_ID) nullptr };
#undef GET_ID
    return configure_options_names;
}

static const crlib_conf_option_t *configuration_options(crlib_conf_t *conf) {
#define ADD_ARR_ELEM(id, flags, ...) { opt_##id, static_cast<crlib_conf_option_flag_t>(flags), __VA_ARGS__ },
  static constexpr const crlib_conf_option_t configure_options[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) {} };
#undef ADD_ARR_ELEM
  return configure_options;
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

static const crlib_extension_t *extensions[] = {
  &restore_data_extension.header,
  &image_constraints_extension.header,
  &image_score_extension.header,
  &description_extension.header,
  nullptr
};

static crlib_extension_t * const *supported_extensions(crlib_conf_t *conf) {
  return extensions;
}

static crlib_extension_t* get_extension(const char *name, size_t size) {
  return find_extension(extensions, name, size);
}

static crlib_api_t api = {
  create_simengine,
  destroy_simengine,
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
