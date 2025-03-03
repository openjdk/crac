/*
 * Copyright (c) 2024-2025, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <process.h>
#include <windows.h>

static constexpr char SLASH = '\\';
static constexpr const char SEP[] = { SLASH, '\0' };

const char *file_separator() {
  return SEP;
}

static inline int isLetter(char c) {
  return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'));
}

// Copy from FileSystemSupport_md.c
static int prefixLength(const char* path) {
  char c0, c1;

  int n = (int)strlen(path);
  if (n == 0) return 0;
  c0 = path[0];
  c1 = (n > 1) ? path[1] : 0;
  if (c0 == SLASH) {
    if (c1 == SLASH) return 2;      /* Absolute UNC pathname "\\\\foo" */
    return 1;                       /* Drive-relative "\\foo" */
  }
  if (isLetter(c0) && (c1 == ':')) {
    if ((n > 2) && (path[2] == SLASH))
      return 3;           /* Absolute local pathname "z:\\foo" */
    return 2;                       /* Directory-relative "z:foo" */
  }
  return 0;                   /* Completely relative */
}

bool is_path_absolute(const char* path) {
  int pl = prefixLength(path);
  return (((pl == 2) && (path[0] == SLASH)) || (pl == 3));
}

char **get_environ() {
  return _environ;
}

bool exec_child_process_and_wait(const char *path, char * const argv[], char * const env[]) {
  // Avoid crashing in _spawnve(), https://bugs.python.org/issue29908
  SetEnvironmentVariable(TEXT("=CRAC_DUMMY"), TEXT("dummy"));
  intptr_t status = _spawnve(_P_WAIT, path, argv, env); // env is inherited by a child process
  return !status;
}

void exec_in_this_process(const char *path, const char *argv[], const char *env[]) {
  // The same problem as above
  SetEnvironmentVariable(TEXT("=CRAC_DUMMY"), TEXT("dummy"));
  _execve(path, argv, env);
}
