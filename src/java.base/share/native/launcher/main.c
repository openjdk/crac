/*
 * Copyright (c) 1995, 2024, Oracle and/or its affiliates. All rights reserved.
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


/*
 * This file contains the main entry point into the launcher code
 * this is the only file which will be repeatedly compiled by other
 * tools. The rest of the files will be linked in.
 */

#include "defines.h"
#include "jli_util.h"
#include "jni.h"

#ifndef WIN32
#include <errno.h>
#endif
#ifdef LINUX
#include <syscall.h>
#endif

/*
 * Entry point.
 */
#ifdef JAVAW

char **__initenv;

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE previnst, LPSTR cmdline, int cmdshow)
{
    const jboolean const_javaw = JNI_TRUE;

    __initenv = _environ;

#else /* JAVAW */

#ifndef _WIN32
#include <stdbool.h>
#include <sys/wait.h>

static bool is_checkpoint = false;
static bool is_restore = false;
static const int crac_min_pid_default = 128;
static int crac_min_pid = 0;
static bool is_min_pid_set = false;

static inline const char *find_option(const char *arg, const char *vmoption) {
    const int len = strlen(vmoption);
    if (0 == strncmp(arg, vmoption, len)) {
        return arg + len;
    }
    return NULL;
}

static void parse_crac(const char *arg) {
    if (!is_checkpoint && find_option(arg, "-XX:CRaCCheckpointTo")) {
        is_checkpoint = true;
    } else if (!is_restore && find_option(arg, "-XX:CRaCRestoreFrom")) {
        is_restore = true;
    } else if (!is_min_pid_set) {
        const char *value = find_option(arg, "-XX:CRaCMinPid=");
        if (value != NULL) {
            crac_min_pid = atoi(value);
            is_min_pid_set = true;
        }
    }
}

static pid_t g_child_pid = -1;

static int wait_for_children() {
    int status = -1;
    pid_t pid;
    do {
        int st = 0;
        pid = wait(&st);
        if (pid == g_child_pid) {
            status = st;
        }
    } while (-1 != pid || ECHILD != errno);

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

static void sighandler(int sig, siginfo_t *info, void *param) {
    if (0 < g_child_pid) {
        kill(g_child_pid, sig);
    }
}

static void setup_sighandler() {
    struct sigaction sigact;
    sigfillset(&sigact.sa_mask);
    sigact.sa_flags = SA_SIGINFO;
    sigact.sa_sigaction = sighandler;

    const int MaxSignalValue = 31;
    for (int sig = 1; sig <= MaxSignalValue; ++sig) {
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
        perror("sigprocmask");
    }
}

static int set_last_pid(int pid) {
#ifdef LINUX
    char buf[11]; // enough for int32
    const int len = snprintf(buf, sizeof(buf), "%d", pid);
    if (0 > len || sizeof(buf) < (size_t)len) {
        return EINVAL;
    }
    const char *last_pid_filename = "/proc/sys/kernel/ns_last_pid";
    const int last_pid_file = open(last_pid_filename, O_WRONLY|O_TRUNC, 0666);
    if (0 > last_pid_file) {
        return errno;
    }
    int res = 0;
    if (len > write(last_pid_file, buf, len)) {
        res = errno;
    }
    close(last_pid_file);
    return res;
#else
    return EPERM;
#endif
}

static void spin_last_pid(int pid) {
    const int MaxSpinCount = pid < 1000 ? 1000 : pid;
    int cnt = MaxSpinCount;
    int child = 0;
    int prev = 0;
    do {
        child = fork();
        if (0 > child) {
            perror("spin_last_pid clone");
            exit(1);
        }
        if (0 == child) {
            exit(0);
        }
        if (child < prev) {
            fprintf(stderr, "%s: Invalid argument (%d)\n", __FUNCTION__, pid);
            exit(1);
        }
        if (0 >= cnt) {
            fprintf(stderr, "%s: Can't reach pid %d, out of try count. Current pid=%d\n", __FUNCTION__, pid, child);
            exit(1);
        }
        prev = child;
        int status;
        if (0 > waitpid(child, &status, 0)) {
            perror("spin_last_pid waitpid");
            exit(1);
        }
        --cnt;
    } while (child < pid);
}
#endif // _WIN32

JNIEXPORT int
main(int argc, char **argv)
{
    const jboolean const_javaw = JNI_FALSE;
#endif /* JAVAW */

    int margc;
    char** margv;
    int jargc;
    const char** jargv = const_jargs;

    jargc = (sizeof(const_jargs) / sizeof(char *)) > 1
        ? sizeof(const_jargs) / sizeof(char *)
        : 0; // ignore the null terminator index

    JLI_InitArgProcessing(jargc > 0, const_disable_argfile);

#ifdef _WIN32
    {
        int i = 0;
        if (getenv(JLDEBUG_ENV_ENTRY) != NULL) {
            printf("Windows original main args:\n");
            for (i = 0 ; i < __argc ; i++) {
                printf("wwwd_args[%d] = %s\n", i, __argv[i]);
            }
        }
    }

    // Obtain the command line in UTF-16, then convert it to ANSI code page
    // without the "best-fit" option
    LPWSTR wcCmdline = GetCommandLineW();
    int mbSize = WideCharToMultiByte(CP_ACP,
        WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
        wcCmdline, -1, NULL, 0, NULL, NULL);
    // If the call to WideCharToMultiByte() fails, it returns 0, which
    // will then make the following JLI_MemAlloc() to issue exit(1)
    LPSTR mbCmdline = JLI_MemAlloc(mbSize);
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
        wcCmdline, -1, mbCmdline, mbSize, NULL, NULL) == 0) {
        perror("command line encoding conversion failure");
        exit(1);
    }

    JLI_CmdToArgs(mbCmdline);
    JLI_MemFree(mbCmdline);

    margc = JLI_GetStdArgc();
    // add one more to mark the end
    margv = (char **)JLI_MemAlloc((margc + 1) * (sizeof(char *)));
    {
        int i = 0;
        StdArg *stdargs = JLI_GetStdArgs();
        for (i = 0 ; i < margc ; i++) {
            margv[i] = stdargs[i].arg;
        }
        margv[i] = NULL;
    }
#else /* *NIXES */
    {
        // accommodate the NULL at the end
        JLI_List args = JLI_List_new(argc + 1);
        int i = 0;

        // Add first arg, which is the app name
        JLI_List_add(args, JLI_StringDup(argv[0]));
        // Append JDK_JAVA_OPTIONS
        if (JLI_AddArgsFromEnvVar(args, JDK_JAVA_OPTIONS)) {
            // JLI_SetTraceLauncher is not called yet
            // Show _JAVA_OPTIONS content along with JDK_JAVA_OPTIONS to aid diagnosis
            if (getenv(JLDEBUG_ENV_ENTRY)) {
                char *tmp = getenv("_JAVA_OPTIONS");
                if (NULL != tmp) {
                    JLI_ReportMessage(ARG_INFO_ENVVAR, "_JAVA_OPTIONS", tmp);
                }
            }
        }
        // Iterate the rest of command line
        for (i = 1; i < argc; i++) {
            parse_crac(argv[i]);
            JLI_List argsInFile = JLI_PreprocessArg(argv[i], JNI_TRUE);
            if (NULL == argsInFile) {
                JLI_List_add(args, JLI_StringDup(argv[i]));
            } else {
                int cnt, idx;
                cnt = argsInFile->size;
                for (idx = 0; idx < cnt; idx++) {
                    JLI_List_add(args, argsInFile->elements[idx]);
                }
                // Shallow free, we reuse the string to avoid copy
                JLI_MemFree(argsInFile->elements);
                JLI_MemFree(argsInFile);
            }
        }
        margc = args->size;
        // add the NULL pointer at argv[argc]
        JLI_List_add(args, NULL);
        margv = args->elements;
    }

    const int is_init = 1 == getpid();
    if (is_init && !is_min_pid_set) {
        crac_min_pid = crac_min_pid_default;
    }
    const int needs_pid_adjust = getpid() < crac_min_pid;
    if (is_checkpoint && (is_init || needs_pid_adjust)) {
        // Move PID value for new processes to a desired value
        // to avoid PID conflicts on restore.
        if (needs_pid_adjust) {
            const int res = set_last_pid(crac_min_pid);
            if (EPERM == res || EACCES == res || EROFS == res) {
                spin_last_pid(crac_min_pid);
            } else if (0 != res) {
                fprintf(stderr, "set_last_pid: %s\n", strerror(res));
                exit(1);
            }
        }

        // Avoid unexpected process completion when checkpointing under docker container run
        // by creating the main process waiting for children before exit.
        g_child_pid = fork();
        if (0 == g_child_pid && needs_pid_adjust && getpid() < crac_min_pid) {
            if (is_min_pid_set) {
                fprintf(stderr, "Error: Can't adjust PID to min PID %d, current PID %d\n", crac_min_pid, (int)getpid());
                exit(1);
            } else {
                fprintf(stderr,
                        "Warning: Can't adjust PID to min PID %d, current PID %d.\n"
                        "This message can be suppressed by '-XX:CRaCMinPid=1' option\n",
                        crac_min_pid, (int)getpid());
            }
        }
        if (0 < g_child_pid) {
            // The main process should forward signals to the child.
            setup_sighandler();
            const int status = wait_for_children();
            exit(status);
        }
    }
#ifdef LINUX
    // /proc filesystem is only on LINUX/*NIX - rseq is not relevant elsewhere anyway
    if (is_checkpoint || is_restore) {
        const char *GLIBC_TUNABLES = "GLIBC_TUNABLES";
        const char *tunables = getenv(GLIBC_TUNABLES);
        // do not try overwrite an existing tunable setting
        if (!tunables || !strstr(tunables, "glibc.pthread.rseq")) {
            char tunables_buf[4096];
            const char *new_tunables = "glibc.pthread.rseq=0";
            if (tunables) {
                int sz = snprintf(tunables_buf, sizeof(tunables_buf), "%s:%s", tunables, new_tunables);
                if (sz < 0 || (int)sizeof(tunables_buf) <= sz) {
                    fprintf(stderr, "Cannot update GLIBC_TUNABLES: does not fit\n");
                    return 1;
                }
                new_tunables = tunables_buf;
            }

            if (setenv(GLIBC_TUNABLES, new_tunables, 1) < 0) {
                perror("setenv GLIBC_TUNABLES");
                return 1;
            }
            execv("/proc/self/exe", argv);
            perror("re-exec");
            return 1;
        }
    }
#endif /* LINUX */
#endif /* not WIN32 */
    return JLI_Launch(margc, margv,
                   jargc, jargv,
                   0, NULL,
                   VERSION_STRING,
                   DOT_VERSION,
                   (const_progname != NULL) ? const_progname : *margv,
                   (const_launcher != NULL) ? const_launcher : *margv,
                   jargc > 0,
                   const_cpwildcard, const_javaw, 0);
}
