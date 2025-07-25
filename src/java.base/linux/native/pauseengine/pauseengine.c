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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define PAUSEENGINE "pauseengine: "

#define RESTORE_SIGNAL   (SIGRTMIN + 2)

static int kickjvm(pid_t jvm, int code) {
    union sigval sv = { .sival_int = code };
    if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
        perror(PAUSEENGINE "sigqueue");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    char* action = argv[1];
    char* imagedir = argv[2];

    char pidpath[1024];
    if (0 > snprintf(pidpath, sizeof(pidpath), "%s/pid", imagedir)) {
        return 1;
    }

    if (!strcmp(action, "checkpoint")) {
        pid_t jvm = getppid();

        FILE *pidfile = fopen(pidpath, "w");
        if (!pidfile) {
            perror(PAUSEENGINE "fopen pidfile");
            kickjvm(jvm, -1);
            return 1;
        }

        fprintf(pidfile, "%d\n", jvm);
        fclose(pidfile);

        fprintf(stderr, PAUSEENGINE "pausing the process, restore from another process to unpause it\n");
    } else if (!strcmp(action, "restore")) {
        FILE *pidfile = fopen(pidpath, "r");
        if (!pidfile) {
            perror(PAUSEENGINE "fopen pidfile");
            return 1;
        }

        pid_t jvm;
        if (1 != fscanf(pidfile, "%d", &jvm)) {
            perror(PAUSEENGINE "fscanf pidfile");
            fclose(pidfile);
            return 1;
        }
        fclose(pidfile);

        char *strid = getenv("CRAC_NEW_ARGS_ID");
        if (kickjvm(jvm, strid ? atoi(strid) : 0)) {
            return 1;
        }

        fprintf(stderr, PAUSEENGINE "successfully unpaused the checkpointed process\n");
    } else {
        fprintf(stderr, PAUSEENGINE "unknown action: %s\n", action);
        return 1;
    }

    return 0;
}
