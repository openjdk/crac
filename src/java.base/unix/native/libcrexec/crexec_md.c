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
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define slash '/'

const char *file_separator() {
  return "/";
}

bool is_path_absolute(const char* path) {
  return path[0] == slash;
}

// Darwin has no "environ" in a dynamic library.
#ifdef __APPLE__
  #include <crt_externs.h>
  #define environ (*_NSGetEnviron())
#else
  extern char** environ;
#endif

char **get_environ() {
  return environ;
}

bool exec_child_process_and_wait(const char *path, char * const argv[], char * const env[]) {
  pid_t pid;
  if (posix_spawn(&pid, path, NULL, NULL, argv, env)) {
    perror("Cannot spawn cracengine");
    return false;
  }

  int status;
  int ret;
  do {
    ret = waitpid(pid, &status, 0);
  } while (ret == -1 && errno == EINTR);

  if (ret == -1 || !WIFEXITED(status)) {
    return false;
  }
  return WEXITSTATUS(status) == 0;
}

void exec_in_this_process(const char *path, const char *argv[], const char *env[]) {
  execve(path, (char **) argv, (char **) env);
}
