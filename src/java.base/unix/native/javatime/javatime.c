/*
 * Copyright (c) 2017, 2021, Azul Systems, Inc. All rights reserved.
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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>

uint64_t nanos(void) {
  struct timespec tp;
  if (0 != clock_gettime(CLOCK_MONOTONIC, &tp)) {
    perror("clock_gettime");
    exit(1);
  }
  return ((uint64_t)tp.tv_sec) * (1000 * 1000 * 1000) + (uint64_t)tp.tv_nsec;
}

uint64_t millis(void) {
  struct timeval time;
  if (0 != gettimeofday(&time, NULL)) {
    perror("gettimeofday");
    exit(1);
  }
  return ((uint64_t)time.tv_sec) * 1000 + ((uint64_t)time.tv_usec)/1000;
}

int main(int argc, char *argv[]) {
  int opt;
  uint64_t (*fn)(void) = nanos;
  while ((opt = getopt(argc, argv, "mn")) != -1) {
  switch (opt) {
    case 'n':
      fn = nanos;
      break;
    case 'm':
      fn = millis;
      break;
    default:
      break;
    }
  }

  uint64_t time = fn();
  char *msg = optind < argc ? argv[optind] : "prestart";
  printf("STARTUPTIME %" PRIu64 " %s\n", time, msg);
  return 0;
}
