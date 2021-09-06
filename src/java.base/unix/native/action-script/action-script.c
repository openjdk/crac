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
