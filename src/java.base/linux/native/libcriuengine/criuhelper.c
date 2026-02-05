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

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define RESTORE_SIGNAL   (SIGRTMIN + 2)

static int g_pid;

static int kickjvm(pid_t jvm, int code) {
    union sigval sv = { .sival_int = code };
    if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
        perror("sigqueue");
        return 1;
    }
    return 0;
}

#define MSGPREFIX ""

static int post_resume(void) {
    char *pidstr = getenv("CRTOOLS_INIT_PID");
    if (!pidstr) {
        fprintf(stderr, MSGPREFIX "cannot find CRTOOLS_INIT_PID env\n");
        return 1;
    }
    int pid = atoi(pidstr);

    char *strid = getenv("CRAC_NEW_ARGS_ID");
    return kickjvm(pid, strid ? atoi(strid) : 0);
}

static void sighandler(int sig, siginfo_t *info, void *uc) {
    if (0 <= g_pid) {
        kill(g_pid, sig);
    }
}

static int restorewait(void) {
    char *pidstr = getenv("CRTOOLS_INIT_PID");
    if (!pidstr) {
        fprintf(stderr, MSGPREFIX "no CRTOOLS_INIT_PID: signals may not be delivered\n");
    }
    g_pid = pidstr ? atoi(pidstr) : -1;

    struct sigaction sigact;
    sigfillset(&sigact.sa_mask);
    sigact.sa_flags = SA_SIGINFO;
    sigact.sa_sigaction = sighandler;

    int sig;
    for (sig = 1; sig <= 31; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP) {
            continue;
        }
        if (-1 == sigaction(sig, &sigact, NULL)) {
            perror("sigaction");
        }
    }

    sigset_t allset;
    sigfillset(&allset);
    if (-1 == sigprocmask(SIG_UNBLOCK, &allset, NULL)) {
        perror(MSGPREFIX "sigprocmask");
    }

    int status;
    int ret;
    do {
        ret = waitpid(g_pid, &status, 0);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        perror(MSGPREFIX "waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        // Try to terminate the current process with the same signal
        // as the child process was terminated
        const int sig = WTERMSIG(status);
        signal(sig, SIG_DFL);
        raise(sig);
        // Signal was ignored, return 128+n as bash does
        // see https://linux.die.net/man/1/bash
        return 128+sig;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    char* action;
    if (argc >= 2 && (action = argv[1])) {
        if (!strcmp(action, "restorewait")) { // called by CRIU --exec-cmd
            return restorewait();
        } else {
            fprintf(stderr, "unknown command-line action: %s\n", action);
            return 1;
        }
    } else if ((action = getenv("CRTOOLS_SCRIPT_ACTION"))) { // called by CRIU --action-script
        if (!strcmp(action, "post-resume")) {
            return post_resume();
        } else {
            // ignore other notifications
            return 0;
        }
    } else {
        fprintf(stderr, "unknown context\n");
    }

    return 1;
}
