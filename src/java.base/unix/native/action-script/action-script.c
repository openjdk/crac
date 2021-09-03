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

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#define RESTORE_SIGNAL   (SIGRTMIN + 2)

#define MSGPREFIX "action-script: "

static int post_resume(void) {
	char *pidstr = getenv("CRTOOLS_INIT_PID");
	if (!pidstr) {
		fprintf(stderr, MSGPREFIX "cannot find CRTOOLS_INIT_PID env\n");
		return 1;
	}
	int pid = atoi(pidstr);

	union sigval sv = { .sival_int = 0 };
	if (-1 == sigqueue(pid, RESTORE_SIGNAL, sv)) {
		perror(MSGPREFIX "sigqueue");
		return 1;
	}

	return 0;
}

static int post_dump(void) {
	char realdir[PATH_MAX];

	char *imgdir = getenv("CRTOOLS_IMAGE_DIR");
	if (!imgdir) {
		fprintf(stderr, MSGPREFIX "cannot find CRTOOLS_IMAGE_DIR env\n");
		return 1;
	}

	if (!realpath(imgdir, realdir)) {
		fprintf(stderr, MSGPREFIX "cannot canonicalize %s: %s\n", imgdir, strerror(errno));
		return 1;
	}

	int dirfd = open(realdir, O_DIRECTORY);
	if (dirfd < 0) {
		fprintf(stderr, MSGPREFIX "can not open image dir %s: %s\n", realdir, strerror(errno));
		return 1;
	}

	int fd = openat(dirfd, "cppath", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, MSGPREFIX "can not open file %s/cppath: %s\n", realdir, strerror(errno));
		return 1;
	}

	if (write(fd, realdir, strlen(realdir)) < 0) {
		fprintf(stderr, MSGPREFIX "can not write %s/cppath: %s\n", realdir, strerror(errno));
		return 1;
	}
	return 0;
}

/** Kicks VM after restore.
 * Started by CRIU on certain phases of restore process. Does nothing after all
 * phases except "post-resume" which is issued after complete restore. Then
 * send signal via with ID attached to restored process. \ref launcher should
 * pass the ID via ZE_CR_NEW_ARGS_ID env variable.
 */
int main(int argc, char *argv[]) {
	char *action = getenv("CRTOOLS_SCRIPT_ACTION");
	if (!action) {
		fprintf(stderr, MSGPREFIX "can not find CRTOOLS_SCRIPT_ACTION env\n");
		return 1;
	}

	if (!strcmp(action, "post-resume")) {
		return post_resume();
	}

	if (!strcmp(action, "post-dump")) {
		return post_dump();
	}

	return 0;
}
