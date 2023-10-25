/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "jni.h"
#include "jvm.h"
#include "crlib.h"

#ifdef LINUX
#include <stdlib.h>
#include <signal.h>
#endif // LINUX

// crexec_md.c
const char *file_separator();
int is_path_absolute(const char *path);
bool path_exists(const char *path);
bool exec_child_process_and_wait(const char *path, const char *argv[]);
void exec_in_this_process(const char *path, const char *argv[]);
void get_current_directory(char *buf, size_t size);

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*x))

static const char* engine = NULL;
static char* arg_str = NULL;
unsigned int argc = 0;
const char* args[32];

static bool parse_crengine(const char *dll_path, const char *crengine) {
    // release possible old copies
  free((char *) engine); // NULL is allowed
  engine = NULL;
  free((char *) arg_str);
  arg_str = NULL;

  if (crengine == NULL) {
    fprintf(stderr, "No CREngine set\n");
    return false;
  }

  char *exec = strdup(crengine);
  if (exec == NULL) {
    perror("Out of memory");
    return false;
  }
  char *comma = strchr(exec, ',');
  if (comma != NULL) {
    *comma = '\0';
    arg_str = strdup(comma + 1);
    if (arg_str == NULL) {
      perror("Out of memory");
      return false;
    }
  }
  if (is_path_absolute(exec)) {
    engine = exec;
  } else {
    char path[PATH_MAX];
    size_t pathlen;
    if (dll_path != NULL) {
      pathlen = strlen(dll_path);
      strncpy(path, dll_path, sizeof(path));
    } else {
      get_current_directory(path, sizeof(path));
      pathlen = strlen(path);
    }
    strncpy(path + pathlen, file_separator(), sizeof(path) - pathlen);
    strncpy(path + pathlen + 1, exec, sizeof(path) - pathlen - 1);
    path[sizeof(path) - 1] = '\0';

    if (!path_exists(path)) {
      fprintf(stderr, "Could not find %s: %s\n", path, strerror(errno));
      return false;
    }
    engine = strdup(path);
    if (engine == NULL) {
      perror("Out of memory");
      return false;
    }
    // we have read and duplicated args from exec, now we can release
    free(exec);
  }
  args[0] = engine;
  argc = 2;

  if (arg_str != NULL) {
    char *arg = arg_str;
    char *target = arg_str;
    bool escaped = false;
    for (char *c = arg; *c != '\0'; ++c) {
      if (argc >= ARRAY_SIZE(args) - 2) {
        fprintf(stderr, "Too many options to CREngine; cannot proceed with these: %s\n", arg);
        return false;
      }
      if (!escaped) {
        switch(*c) {
        case '\\':
          escaped = true;
          continue; // for
        case ',':
          *target++ = '\0';
          args[argc++] = arg;
          arg = target;
          continue; // for
        }
      }
      escaped = false;
      *target++ = *c;
    }
    *target = '\0';
    args[argc++] = arg;
    args[argc] = NULL;
  }
  return true;
}

static void add_crengine_arg(const char *arg) {
  if (argc >= ARRAY_SIZE(args) - 1) {
      fprintf(stderr, "Too many options to CREngine; cannot add %s\n", arg);
      return;
  }
  args[argc++] = arg;
  args[argc] = NULL;
}

static bool checkpoint(crlib_api *api) {
  if (!parse_crengine(api->dll_path, api->args)) {
    return false;
  }
  args[1] = "checkpoint";
  if (api->leave_running) {
#ifdef LINUX
    if (setenv("CRAC_CRIU_LEAVE_RUNNING", "", true)) {
      perror("Cannot set CRAC_CRIU_LEAVE_RUNNING");
    }
#endif // LINUX
  }
  add_crengine_arg(api->image_location);
  if (!exec_child_process_and_wait(engine, args)) {
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
  api->shmid = info.si_int;
#endif // LINUX
  return true;
}

static void restore(crlib_api *api) {
  parse_crengine(api->library_path, api->args);
  args[1] = "restore";
  add_crengine_arg(api->image_location);

#ifdef LINUX
  char strid[32];
  snprintf(strid, sizeof(strid), "%d", api->shmid);
  setenv("CRAC_NEW_ARGS_ID", strid, true);
#endif // LINUX

  exec_in_this_process(engine, args);
  fprintf(stderr, "Restore failed\n");
}

JNIEXPORT bool JNICALL CRLIB_API_INIT(int api_version, crlib_api *api, size_t api_size) {
  if (api_version != CRLIB_API_VERSION) {
    return false;
  }
  if (sizeof(crlib_api) != api_size) {
    return false; // wrong bitness?
  }
  memset(api, 0, sizeof(*api));
  api->checkpoint = checkpoint;
  api->restore = restore;
  return true;
}