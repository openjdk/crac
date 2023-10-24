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

// crexec_md.c
const char *file_separator();
int is_path_absolute(const char *path);
bool path_exists(const char *path);
bool exec_child_process_and_wait(const char *path, const char *argv[]);
void exec_in_this_process(const char *path, const char *argv[]);

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*x))

static const char *engine_path = NULL;

static const char* engine = NULL;
static char* arg_str = NULL;
unsigned int argc = 0;
const char* args[32];

JNIEXPORT void JNICALL set_engine_path(const char* path) {
  engine_path = strdup(path);
  if (engine_path == NULL) {
    perror("Out of memory");
  }
}

static bool parse_crengine(const char *crengine) {
    // release possible old copies
  free((char *) engine); // NULL is allowed
  engine = NULL;
  free((char *) arg_str);
  arg_str = NULL;

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
    size_t pathlen = strlen(engine_path);
    strncpy(path, engine_path, sizeof(path));
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

JNIEXPORT bool JNICALL checkpoint(const char *image_location, const char *crengine) {
  parse_crengine(crengine);
  args[1] = "checkpoint";
  add_crengine_arg(image_location);
  return exec_child_process_and_wait(engine, args);
}

#if 0
static void crengine_raise_restore() {
  int pid = os::current_process_id();
  union sigval val = {
    .sival_int = pid
  };
  if (sigqueue(pid, RESTORE_SIGNAL, val)) {
    perror("Cannot raise restore signal");
  }
}
#endif


JNIEXPORT void JNICALL restore(const char *image_location, const char *crengine) {
  parse_crengine(crengine);
  args[1] = "restore";
  add_crengine_arg(image_location);
  exec_in_this_process(engine, args);
  fprintf(stderr, "Restore failed\n");
}