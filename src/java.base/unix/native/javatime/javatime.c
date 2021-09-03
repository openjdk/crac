// Copyright 2017-2020 Azul Systems, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

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
