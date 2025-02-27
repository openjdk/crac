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
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crlib/crlib.h"
#include "crlib/crlib_restore_data.h"
#include "hashtable.h"
#include "jni.h"

#ifdef LINUX
#include <signal.h>

#include "jvm.h"
#endif // LINUX

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

#define CREXEC "crexec: "

// When adding a new option also add its description into the help message and
// ensure the proper default value is set for it on configuration init. Also
// consider checking for its inappropriate use on checkpoint/restore.
//
// More frequently used options should go first: this means they'll be the first
// to be placed in the options hash table which is implemented so that the keys
// added first are faster to find.
#define CONFIGURE_OPTIONS(OPT) \
  OPT(exec_location, "exec_location") \
  OPT(image_location, "image_location") \
  OPT(keep_running, "keep_running") \
  OPT(direct_map, "direct_map") \
  OPT(args, "args") \
  OPT(help, "help") \

#define DEFINE_OPT(id, str) static const char opt_##id[] = str;
CONFIGURE_OPTIONS(DEFINE_OPT)
#undef DEFINE_OPT
#define ADD_ARR_ELEM(id, str) opt_##id,
static const char *configure_options[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) };
#undef ADD_ARR_ELEM

// Indices of argv arrag members
enum Argv {
  ARGV_EXEC_LOCATION,
  ARGV_ACTION,
  ARGV_IMAGE_LOCATION,
  ARGV_FREE, // First index for user-provided arguments
  ARGV_LAST = 31,
};

struct crlib_conf {
  hashtable_t *options;

  bool keep_running;
  bool direct_map;
  int restore_data;

  unsigned int argc;
  const char *argv[ARGV_LAST + 2]; // Last element is required to be null
};

// crexec_md.c
const char *file_separator(void);
bool is_path_absolute(const char *path);
bool exec_child_process_and_wait(const char *path, char * const argv[], char * const env[]);
char **get_environ(void);
void exec_in_this_process(const char *path, const char *argv[], const char *env[]);

typedef bool (*configure_func)(crlib_conf_t *, const char *value);

static bool configure_help(crlib_conf_t *conf, const char *ignored) {
  // Internal options which are expected to be set by the program crexec is linked against are
  // omitted from the print since users are not supposed to pass them directly:
  // * image_location=<path> — path to a directory with checkpoint/restore files.
  // * exec_location=<path> — path to the engine executable.
  const int ret = printf(
    "\n"
    "crexec — pseudo-CRaC-engine used to relay data from JVM to a \"real\" engine implemented as "
    "an executable (instead of a library).\n"
    "The engine executable is expected to have CRaC-CRIU-like CLI. Support of the options below "
    "also depends on the engine executable.\n"
    "\n"
    "Configuration options:\n"
    "* keep_running=<true/false> (default: false) — keep the process running after the checkpoint "
    "or kill it.\n"
    "* direct_map=<true/false> (default: false) — on restore, map process data directly from saved "
    "files. This may speedup the restore but the resulting process will not be the same as before "
    "the checkpoint.\n"
    "* args=<string> (default: \"\") — free space-separated arguments passed directly to the "
    "engine executable, e.g. \"--arg1 --arg2 --arg3\".\n"
    "* help — print this message.\n"
    "\n"
  );
  return ret > 0;
}

static char *strdup_checked(const char *src) {
  char *res = strdup(src);
  if (res == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
  }
  return res;
}

static bool parse_bool(const char *str, bool *result) {
  if (strcmp(str, "true") == 0) {
    *result = true;
    return true;
  }
  if (strcmp(str, "false") == 0) {
    *result = false;
    return true;
  }
  fprintf(stderr, CREXEC "expected '%s' to be either 'true' or 'false'\n", str);
  return false;
}

static bool configure_exec_location(crlib_conf_t *conf, const char *exec_location) {
  if (!is_path_absolute(exec_location)) {
    fprintf(stderr, CREXEC "expected absolute path: %s\n", exec_location);
    return false;
  }
  free((char *) conf->argv[ARGV_EXEC_LOCATION]);
  conf->argv[ARGV_EXEC_LOCATION] = strdup_checked(exec_location);
  if (conf->argv[ARGV_EXEC_LOCATION] == NULL) {
    return false;
  }
  return true;
}

static bool configure_image_location(crlib_conf_t *conf, const char *image_location) {
  free((char *) conf->argv[ARGV_IMAGE_LOCATION]);
  conf->argv[ARGV_IMAGE_LOCATION] = strdup_checked(image_location);
  return conf->argv[ARGV_IMAGE_LOCATION] != NULL;
}

static bool configure_keep_running(crlib_conf_t *conf, const char *keep_running_str) {
  return parse_bool(keep_running_str, &conf->keep_running);
}

static bool configure_direct_map(crlib_conf_t *conf, const char *direct_map_str) {
  return parse_bool(direct_map_str, &conf->direct_map);
}

static bool configure_args(crlib_conf_t *conf, const char *args) {
  free((char *) conf->argv[ARGV_FREE]);
  char *arg = strdup_checked(args);
  if (arg == NULL) {
    conf->argv[ARGV_FREE] = NULL;
    return false;
  }

  assert(ARGV_FREE <= ARGV_LAST);
  for (int i = ARGV_FREE; i <= ARGV_LAST && arg[0] != '\0'; i++) {
    conf->argv[i] = arg;
    char * const delim = strchr(arg, ' ');
    if (delim != NULL) {
      *delim = '\0';
      arg = delim + 1;
    } else {
      arg = "\0";
    }
  }

  if (arg[0] != '\0') {
    fprintf(stderr, CREXEC "too many free arguments, at most %i are allowed\n",
            (ARGV_LAST - ARGV_FREE) + 1);
    free((char *) conf->argv[ARGV_FREE]);
    conf->argv[ARGV_FREE] = NULL;
    return false;
  }

  return true;
}

static bool can_configure(crlib_conf_t *conf, const char *key) {
  assert(key != NULL);
  return hashtable_contains(conf->options, key);
}

static bool configure(crlib_conf_t *conf, const char *key, const char *value) {
  assert(key != NULL && value != NULL);
  const configure_func func = (configure_func) hashtable_get(conf->options, key);
  if (func != NULL) {
    return func(conf, value);
  }
  fprintf(stderr, CREXEC "unknown configure option: %s\n", key);
  return false;
}

static bool set_restore_data(crlib_conf_t *conf, const void *data, size_t size) {
  if (size != sizeof(conf->restore_data)) {
    fprintf(stderr, CREXEC "unsupported size of restore data: %zu — only %zu is supported\n",
            size, sizeof(conf->restore_data));
    return false;
  }
  conf->restore_data = *(int *) data;
  return true;
}

static size_t get_restore_data(crlib_conf_t *conf, void *buf, size_t size) {
  if (size < sizeof(conf->restore_data)) {
    fprintf(stderr, CREXEC "can only provide >= %zu bytes of restore data but %zu was requested\n",
            sizeof(conf->restore_data), size);
    return 0;
  }
  *(int *) buf = conf->restore_data;
  return sizeof(conf->restore_data);
}

static crlib_restore_data_t restore_data_extension = {
  .header = {
    .name = CRLIB_EXTENSION_RESTORE_DATA_NAME,
    .size = sizeof(crlib_restore_data_t)
  },
  .set_restore_data = set_restore_data,
  .get_restore_data = get_restore_data
};

static const crlib_extension_t *extensions[] = { &restore_data_extension.header };

static const crlib_extension_t *get_extension(const char *name, size_t size) {
	for (size_t i = 0; i < ARRAY_SIZE(extensions); i++) {
    const crlib_extension_t *ext = extensions[i];
		if (strcmp(name, ext->name) == 0) {
      if (size <= ext->size) {
        return ext;
      }
      return NULL;
		}
	}
	return NULL;
}

static void destroy_conf(crlib_conf_t *conf) {
  if (conf == NULL) {
    return;
  }
  hashtable_destroy(conf->options);
  for (int i = 0; i <= ARGV_FREE /* all free args are allocated together */; i++) {
    if (i != ARGV_ACTION) { // Action is a static string
      free((char*) conf->argv[i]);
    }
  }
  free(conf);
}

static crlib_conf_t *create_conf() {
  crlib_conf_t * const conf = (crlib_conf_t *) malloc(sizeof(crlib_conf_t));
  if (conf == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    return NULL;
  }
  memset(conf, 0, sizeof(*conf));

  conf->options = hashtable_create(configure_options, ARRAY_SIZE(configure_options));
  if (conf->options == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    destroy_conf(conf);
    return NULL;
  }
#define PUT_HANDLER(id, str) hashtable_put(conf->options, opt_##id, configure_##id);
  CONFIGURE_OPTIONS(PUT_HANDLER)
#undef PUT_HANDLER

  return conf;
}

static void free_environ(char **env) {
  for (size_t i = 0; env[i] != NULL; ++i) {
    free(env[i]);
  }
  free(env);
}

static char **copy_environ(char * const *env) {
  size_t len = 0;
  for (; env[len] != NULL; len++) {}

  char ** const new_env = malloc((len + 1) * sizeof(char *));
  if (new_env == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    return NULL;
  }

  for (size_t i = 0; i < len; i++) {
    new_env[i] = strdup(env[i]);
    if (new_env[i] == NULL) {
      fprintf(stderr, CREXEC "out of memory\n");
      free_environ(new_env);
      return NULL;
    }
  }
  new_env[len] = NULL;

  return new_env;
}

static char **set_env_var(char **env, const char *var, const char *value) {
  const size_t str_size = strlen(var) + strlen("=") + strlen(value) + 1;
  char * const str = malloc(sizeof(char) * str_size);
  if (str == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    return NULL;
  }
  if (snprintf(str, str_size, "%s=%s", var, value) != (int) str_size - 1) {
    perror(CREXEC "snprintf env var");
    free(str);
    return NULL;
  }

  size_t len = 0;
  for (; env[len] != NULL; len++) {}

  char ** const new_env = realloc(env, (len + 2) * sizeof(char *));
  if (new_env == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    free(str);
    return NULL;
  }

  new_env[len] = str;
  new_env[len + 1] = NULL;

  return new_env;
}

static char **add_criu_option(char **env, const char *opt) {
  static const char CRAC_CRIU_OPTS[] = "CRAC_CRIU_OPTS";
  static const size_t CRAC_CRIU_OPTS_LEN = ARRAY_SIZE(CRAC_CRIU_OPTS) - 1;

  bool opts_found = false;
  size_t opts_index = 0;
  for (; env[opts_index] != NULL; opts_index++) {
    if (strcmp(env[opts_index], CRAC_CRIU_OPTS) == 0 && env[opts_index][CRAC_CRIU_OPTS_LEN] == '=') {
      opts_found = true;
      break;
    }
  }

  if (!opts_found) {
    return set_env_var(env, CRAC_CRIU_OPTS, opt);
  }

  if (strstr(env[opts_index] + CRAC_CRIU_OPTS_LEN + 1, opt) != NULL) {
    return env;
  }

  const size_t new_opts_size = strlen(env[opts_index]) + strlen(" ") + strlen(opt) + 1;
  char * const new_opts = malloc(new_opts_size * sizeof(char));
  if (new_opts == NULL) {
    fprintf(stderr, CREXEC "out of memory\n");
    return NULL;
  }
  if (snprintf(new_opts, new_opts_size, "%s %s", env[opts_index], opt) != (int) new_opts_size - 1) {
    perror(CREXEC "snprintf CRAC_CRIU_OPTS (append)");
    free(new_opts);
    return NULL;
  }
  free(env[opts_index]);
  env[opts_index] = new_opts;

  return env;
}

static bool checkpoint(crlib_conf_t *conf) {
  if (conf->argv[ARGV_EXEC_LOCATION] == NULL) {
    fprintf(stderr, CREXEC "%s must be set before checkpoint\n", opt_exec_location);
    return false;
  }
  if (conf->argv[ARGV_IMAGE_LOCATION] == NULL) {
    fprintf(stderr, CREXEC "%s must be set before checkpoint\n", opt_image_location);
    return false;
  }
  conf->argv[ARGV_ACTION] = "checkpoint";

  if (conf->direct_map) {
    fprintf(stderr, CREXEC "%s has no effect on checkpoint\n", opt_direct_map);
  }

  char **env = copy_environ(get_environ());
  if (env == NULL) {
    return false;
  }
  if (conf->keep_running) {
    char ** const new_env = set_env_var(env, "CRAC_CRIU_LEAVE_RUNNING", "");
    if (new_env == NULL) {
      free_environ(env);
      return false;
    }
    env = new_env;
  }

  const bool ok = exec_child_process_and_wait(conf->argv[ARGV_EXEC_LOCATION],
                                              (char **) conf->argv, env);
  free_environ(env);
  if (!ok) {
    return false;
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
  conf->restore_data = info.si_int;
#endif // LINUX

  return true;
}

static void restore(crlib_conf_t *conf) {
  if (conf->argv[ARGV_EXEC_LOCATION] == NULL) {
    fprintf(stderr, CREXEC "%s must be set before restore\n", opt_exec_location);
    return;
  }
  if (conf->argv[ARGV_IMAGE_LOCATION] == NULL) {
    fprintf(stderr, CREXEC "%s must be set before restore\n", opt_image_location);
    return;
  }
  conf->argv[ARGV_ACTION] = "restore";

  if (conf->keep_running) {
    fprintf(stderr, CREXEC "%s has no effect on restore\n", opt_keep_running);
  }

  char **env = copy_environ(get_environ());
  if (env == NULL) {
    return;
  }

  {
    char ** const new_env = set_env_var(env, "CRAC_CRIU_LEAVE_RUNNING", "");
    if (new_env == NULL) {
      free_environ(env);
      return;
    }
    env = new_env;
  }

  char restore_data_str[32];
  if (snprintf(restore_data_str, sizeof(restore_data_str), "%i", conf->restore_data) >
      (int) sizeof(restore_data_str) - 1) {
    perror(CREXEC "snprintf restore data");
    free_environ(env);
    return;
  }
  {
    char ** const new_env = set_env_var(env, "CRAC_NEW_ARGS_ID", restore_data_str);
    if (new_env == NULL) {
      free_environ(env);
      return;
    }
    env = new_env;
  }

  if (!conf->direct_map) {
    char ** const new_env = add_criu_option(env, "--no-mmap-page-image");
    if (new_env == NULL) {
      free_environ(env);
      return;
    }
    env = new_env;
  }

  exec_in_this_process(conf->argv[ARGV_EXEC_LOCATION], conf->argv, (const char **) env);

  free_environ(env); // shouldn't be needed
  fprintf(stderr, CREXEC "restore failed\n");
}

static struct crlib_api api = {
  .create_conf = create_conf,
  .destroy_conf = destroy_conf,
  .checkpoint = checkpoint,
  .restore = restore,
  .can_configure = can_configure,
  .configure = configure,
  .get_extension = get_extension,
};

JNIEXPORT crlib_api_t *CRLIB_API(int api_version, size_t api_size) {
  if (api_version != CRLIB_API_VERSION) {
    return NULL;
  }
  if (sizeof(crlib_api_t) < api_size) {
    return NULL;
  }
  return &api;
}
