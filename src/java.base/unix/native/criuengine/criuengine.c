
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

#define RESTORE_SIGNAL   (SIGRTMIN + 2)

#define PERFDATA_NAME "perfdata"

static int g_pid;

static int kickjvm(pid_t jvm, int code) {
	union sigval sv = { .sival_int = code };
	if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
		perror("sigqueue");
		return 1;
	}
	return 0;
}

static int checkpoint(pid_t jvm, const char *basedir, const char *self, const char *criu, const char *imagedir) {
	if (fork()) {
		// main process
		wait(NULL);
		return 0;
	}

	pid_t parent_before = getpid();

	// child
	if (fork()) {
		exit(0);
	}

	// grand-child
	pid_t parent = getppid();
	int tries = 300;
	while (parent != 1 && 0 < tries--) {
		usleep(10);
		parent = getppid();
	}

	if (parent == parent_before) {
		fprintf(stderr, "can't move out of JVM process hierarchy");
		kickjvm(jvm, -1);
		exit(0);
	}


	char jvmpidchar[32];
	snprintf(jvmpidchar, sizeof(jvmpidchar), "%d", jvm);
	
	pid_t child = fork();
	if (!child) {
		execl(criu, criu, "dump",
				"-t", jvmpidchar,
				"-D", imagedir,
				"--action-script", self, 
				"--shell-job",
				"-v4", "-o", "dump4.log", // -D without -W makes criu cd to image dir for logs
				NULL);
		perror("exec criu");
		exit(1);
	}

	int status;
	if (child != wait(&status) || !WIFEXITED(status) || WEXITSTATUS(status)) {

		kickjvm(jvm, -1);
	}
	exit(0);
}

static int restore(const char *basedir, const char *self, const char *criu, const char *imagedir) {
	char *cppathpath;
	if (-1 == asprintf(&cppathpath, "%s/cppath", imagedir)) {
		return 1;
	}

	char cppath[PATH_MAX];
	cppath[0] = '\0';
	FILE *cppathfile = fopen(cppathpath, "r");
	if (!cppathfile) {
		perror("open cppath");
		return 1;
	}

	if (fgets(cppath, sizeof(cppath), cppathfile)) {
		return 1;
	}
	fclose(cppathfile);

	char *inherit_perfdata = NULL;
	char *perfdatapath;
	if (-1 == asprintf(&perfdatapath, "%s/" PERFDATA_NAME, imagedir)) {
		return 1;
	}
	int perfdatafd = open(perfdatapath, O_RDWR);
	if (0 < perfdatafd) {
		if (-1 == asprintf(&inherit_perfdata, "fd[%d]:%s/" PERFDATA_NAME,
				perfdatafd,
				cppath[0] == '/' ? cppath + 1 : cppath)) {
			return 1;
		}
	}

#define CRIU_HEAD (char*)criu, "restore"
#define CRIU_TAIL "-W", ".", \
		"--shell-job", \
		"--action-script", self, \
		"-D", imagedir, \
		"-v1", \
		"--exec-cmd", "--", self, "restorewait", \
		NULL

	if (inherit_perfdata) {
		execl(criu, CRIU_HEAD, "--inherit-fd", inherit_perfdata, CRIU_TAIL);
	} else {
		execl(criu, CRIU_HEAD, CRIU_TAIL);
	}
	perror("exec criu");
	return 1;
}

#define MSGPREFIX ""

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

    return 1;
}

int main(int argc, char *argv[]) {
	char* action;
	if ((action = argv[1])) {
		char* imagedir = argv[2];

		char *basedir = dirname(strdup(argv[0]));
		char *criu;
		if (-1 == asprintf(&criu, "%s/criu", basedir)) {
			return 1;
		}
		if (!strcmp(action, "checkpoint")) {
			pid_t jvm = getppid();
			return checkpoint(jvm, basedir, argv[0], criu, imagedir);
		} else if (!strcmp(action, "restore")) {
			return restore(basedir, argv[0], criu, imagedir);
		} else if (!strcmp(action, "restorewait")) { // called by CRIU --exec-cmd
			return restorewait();
		} else {
			fprintf(stderr, "unknown command-line action: %s\n", action);
			return 1;
		}
	} else if ((action = getenv("CRTOOLS_SCRIPT_ACTION"))) { // called by CRIU --action-script
		if (!strcmp(action, "post-resume")) {
			return post_resume();
		} else if (!strcmp(action, "post-dump")) {
			return post_dump();
		} else {
			// ignore other notifications
			return 0;
		}
	} else {
		fprintf(stderr, "unknown context\n");
	}
		
	return 1;
}
