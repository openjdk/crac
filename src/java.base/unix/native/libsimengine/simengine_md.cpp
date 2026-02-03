/*
 * Copyright (c) 2021,2026, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021,2026, Oracle and/or its affiliates. All rights reserved.
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
#include <signal.h>
#include <string.h>

#include "crcommon.hpp"

// pause is supported only on Linux.
// OSX does not define SIGRTMIN nor sigwaitinfo
// FIXME: it would be better to implement this using another (standard) IPC mechanism, e.g. named pipe
#ifdef LINUX

#define RESTORE_SIGNAL (SIGRTMIN + 2)

int kickjvm(pid_t jvm, int code) {
    union sigval sv = { .sival_int = code };
    if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
        LOG("sigqueue: %s", strerror(errno));
        return 1;
    }
    return 0;
}

int waitjvm() {
    siginfo_t info;
    sigset_t waitmask;
    sigemptyset(&waitmask);
    sigaddset(&waitmask, RESTORE_SIGNAL);

    int sig;
    do {
        sig = sigwaitinfo(&waitmask, &info);
    } while (sig == -1 && errno == EINTR);

    if (info.si_code != SI_QUEUE) {
        return -1;
    }
    return info.si_int;
}

#endif // LINUX
