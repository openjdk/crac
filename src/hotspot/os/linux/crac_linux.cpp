/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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

// no precompiled headers
#include "jvm.h"
#include "perfMemory_linux.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/crac.hpp"
#include "runtime/os.hpp"
#include "utilities/growableArray.hpp"
#include "logging/log.hpp"
#include "classfile/classLoader.hpp"

class FdsInfo {
public:

  enum state_t {
    INVALID = -3,
    CLOSED = -2,
    ROOT = -1,
    DUP_OF_0 = 0,
    // ...
  };

  enum mark_t {
    M_CANT_RESTORE = 1 << 0,
  };

private:
  struct fdinfo {
    int fd;
    struct stat stat;
    state_t state;
    unsigned mark;

    int flags;
  };

  // params are indices into _fdinfos
  bool same_fd(int i1, int i2);

  bool _inited;
  GrowableArray<fdinfo> _fdinfos;

  void assert_mark(int i) {
    assert(_inited, "");
    assert(i < _fdinfos.length(), "");
    assert(_fdinfos.at(i).state != CLOSED, "");
  }

public:
  void initialize();

  int len() { return _fdinfos.length(); }

  state_t get_state(int i) {
    assert(_inited, "");
    assert(i < _fdinfos.length(), "");
    return _fdinfos.at(i).state;
  }

  state_t find_state(int fd, state_t orstate) {
    for (int i = 0; i < _fdinfos.length(); ++i) {
      fdinfo *info = _fdinfos.adr_at(i);
      if (info->fd == fd) {
        return info->state;
      }
    }
    return orstate;
  }

  int get_fd(int i) {
    assert(_inited, "");
    assert(i < _fdinfos.length(), "");
    return _fdinfos.at(i).fd;
  }

  struct stat* get_stat(int i) {
    assert(_inited, "");
    assert(i < _fdinfos.length(), "");
    return &_fdinfos.at(i).stat;
  }

  FdsInfo(bool do_init = true) :
    _inited(false),
    _fdinfos(16, mtInternal)
  {
    if (do_init) {
      initialize();
    }
  }
};

static FdsInfo _vm_inited_fds(false);

/* taken from criu, that took this from kernel */
#define NFS_PREF ".nfs"
#define NFS_PREF_LEN ((unsigned)sizeof(NFS_PREF) - 1)
#define NFS_FILEID_LEN ((unsigned)sizeof(uint64_t) << 1)
#define NFS_COUNTER_LEN ((unsigned)sizeof(unsigned int) << 1)
#define NFS_LEN (NFS_PREF_LEN + NFS_FILEID_LEN + NFS_COUNTER_LEN)
static bool nfs_silly_rename(char* path) {
  char *sep = strrchr(path, '/');
  char *base = sep ? sep + 1 : path;
  if (strncmp(base, NFS_PREF, NFS_PREF_LEN)) {
    return false;
  }
  for (unsigned i = NFS_PREF_LEN; i < NFS_LEN; ++i) {
    if (!isxdigit(base[i])) {
      return false;
    }
  }
  return true;
}

static int readfdlink(int fd, char *link, size_t len) {
  char fdpath[64];
  snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
  int ret = readlink(fdpath, link, len);
  if (ret == -1) {
    return ret;
  }
  link[(unsigned)ret < len ? ret : len - 1] = '\0';
  return ret;
}

static bool same_stat(struct stat* st1, struct stat* st2) {
  return st1->st_dev == st2->st_dev &&
         st1->st_ino == st2->st_ino;
}

bool FdsInfo::same_fd(int i1, int i2) {
  assert(i1 < _fdinfos.length(), "");
  assert(i2 < _fdinfos.length(), "");
  fdinfo *fi1 = _fdinfos.adr_at(i1);
  fdinfo *fi2 = _fdinfos.adr_at(i2);
  if (!same_stat(&fi1->stat, &fi2->stat)) {
    return false;
  }

  int flags1 = fcntl(fi1->fd, F_GETFL);
  int flags2 = fcntl(fi2->fd, F_GETFL);
  if (flags1 != flags2) {
    return false;
  }

  const int test_flag = O_NONBLOCK;
  const int new_flags1 = flags1 ^ test_flag;
  fcntl(fi1->fd, F_SETFL, new_flags1);
  if (fcntl(fi1->fd, F_GETFL) != new_flags1) {
    // flag write ignored or handled differently,
    // don't know what to do
    return false;
  }

  const int new_flags2 = fcntl(fi2->fd, F_GETFL);
  const bool are_same = new_flags1 == new_flags2;

  fcntl(fi2->fd, flags1);

  return are_same;
}

void FdsInfo::initialize() {
  assert(!_inited, "should be called only once");

  char path[PATH_MAX];
  struct dirent *dp;

  DIR *dir = opendir("/proc/self/fd");
  int dfd = dirfd(dir);
  while (dp = readdir(dir)) {
    if (dp->d_name[0] == '.') {
      // skip "." and ".."
      continue;
    }
    fdinfo info;
    info.fd = atoi(dp->d_name);
    if (info.fd == dfd) {
      continue;
    }
    int r = fstat(info.fd, &info.stat);
    if (r == -1) {
      info.state = CLOSED;
      continue;
    }
    info.state = ROOT; // can be changed to DUP_OF_0 + N below
    info.mark = 0;
    _fdinfos.append(info);
  }
  closedir(dir);
  _inited = true;

  for (int i = 0; i < _fdinfos.length(); ++i) {
    fdinfo *info = _fdinfos.adr_at(i);
    for (int j = 0; j < i; ++j) {
      if (get_state(j) == ROOT && same_fd(i, j)) {
        info->state = (state_t)(DUP_OF_0 + j);
        break;
      }
    }

    if (info->state == ROOT) {
      char fdpath[PATH_MAX];
      int r = readfdlink(info->fd, fdpath, sizeof(fdpath));
      guarantee(-1 != r, "can't stat fd");
      if (info->stat.st_nlink == 0 ||
          strstr(fdpath, "(deleted)") ||
          nfs_silly_rename(fdpath)) {
        info->mark |= FdsInfo::M_CANT_RESTORE;
      }
    }
  }
}

static const char* stat2strtype(mode_t mode) {
  switch (mode & S_IFMT) {
  case S_IFSOCK: return "socket";
  case S_IFLNK:  return "symlink";
  case S_IFREG:  return "regular";
  case S_IFBLK:  return "block";
  case S_IFDIR:  return "directory";
  case S_IFCHR:  return "character";
  case S_IFIFO:  return "fifo";
  default:       break;
  }
  return "unknown";
}

static int stat2stfail(mode_t mode) {
  switch (mode & S_IFMT) {
  case S_IFSOCK:
    return JVM_CR_FAIL_SOCK;
  case S_IFLNK:
  case S_IFREG:
  case S_IFBLK:
  case S_IFDIR:
  case S_IFCHR:
    return JVM_CR_FAIL_FILE;
  case S_IFIFO:
    return JVM_CR_FAIL_PIPE;
  default:
    break;
  }
  return JVM_CR_FAIL;
}

// If checkpoint is called throught the API, jcmd operation and jcmd output doesn't exist.
bool VM_Crac::is_socket_from_jcmd(int sock) {
  if (_attach_op == NULL)
    return false;
  int sock_fd = _attach_op->socket();
  return sock == sock_fd;
}

void VM_Crac::report_ok_to_jcmd_if_any() {
  if (_attach_op == NULL)
    return;
  bufferedStream* buf = static_cast<bufferedStream*>(_ostream);
  _attach_op->effectively_complete_raw(JNI_OK, buf);
  // redirect any further output to console
  _ostream = tty;
}

bool VM_Crac::check_fds() {

  AttachListener::abort();

  FdsInfo fds;

  bool ok = true;

  for (int i = 0; i < fds.len(); ++i) {
    if (fds.get_state(i) == FdsInfo::CLOSED) {
      continue;
    }
    int fd = fds.get_fd(i);

    char detailsbuf[PATH_MAX];
    struct stat* st = fds.get_stat(i);
    const char* type = stat2strtype(st->st_mode);
    int linkret = readfdlink(fd, detailsbuf, sizeof(detailsbuf));
    const char* details = 0 < linkret ? detailsbuf : "";
    print_resources("JVM: FD fd=%d type=%s path=\"%s\" ", fd, type, details);

    if (is_claimed_fd(fd)) {
      print_resources("OK: claimed by java code\n");
      continue;
    }

    if (_vm_inited_fds.find_state(fd, FdsInfo::CLOSED) != FdsInfo::CLOSED) {
      print_resources("OK: inherited from process env\n");
      continue;
    }

    if (S_ISSOCK(st->st_mode)) {
      if (is_socket_from_jcmd(fd)){
        print_resources("OK: jcmd socket\n");
        continue;
      }
    }

    print_resources("BAD: opened by application\n");
    ok = false;

    const int maxinfo = 64;
    size_t buflen = strlen(details) + maxinfo;
    char* msg = NEW_C_HEAP_ARRAY(char, buflen, mtInternal);
    int len = snprintf(msg, buflen, "FD fd=%d type=%s path=%s", fd, type, detailsbuf);
    msg[len < 0 ? 0 : ((size_t) len >= buflen ? buflen - 1 : len)] = '\0';
    _failures->append(CracFailDep(stat2stfail(st->st_mode & S_IFMT), msg));
  }

  return ok;
}

bool VM_Crac::memory_checkpoint() {
  return PerfMemoryLinux::checkpoint(CRaCCheckpointTo);
}

void VM_Crac::memory_restore() {
  PerfMemoryLinux::restore();
}

static char modules_path[JVM_MAXPATHLEN] = { '\0' };

static bool is_fd_ignored(int fd, const char *path) {
  const char *list = CRaCIgnoredFileDescriptors;
  while (list && *list) {
    const char *end = strchr(list, ',');
    if (!end) {
      end = list + strlen(list);
    }
    char *invalid;
    int ignored_fd = strtol(list, &invalid, 10);
    if (invalid == end) { // entry was integer -> file descriptor
      if (fd == ignored_fd) {
        log_trace(os)("CRaC not closing file descriptor %d (%s) as it is marked as ignored.", fd, path);
        return true;
      }
    } else { // interpret entry as path
      int path_len = path ? strlen(path) : -1;
      if (path_len != -1 && path_len == end - list && !strncmp(path, list, end - list)) {
        log_trace(os)("CRaC not closing file descriptor %d (%s) as it is marked as ignored.", fd, path);
        return true;
      }
    }
    if (*end) {
      list = end + 1;
    } else {
      break;
    }
  }

  if (os::same_files(modules_path, path)) {
    // Path to the modules directory is opened early when JVM is booted up and won't be closed.
    // We can ignore this for purposes of CRaC.
    return true;
  }

  return false;
}

static void close_extra_descriptors() {
  // Path to the modules directory is opened early when JVM is booted up and won't be closed.
  // We can ignore this for purposes of CRaC.
  if (modules_path[0] == '\0') {
    const char* fileSep = os::file_separator();
    jio_snprintf(modules_path, JVM_MAXPATHLEN, "%s%slib%s" MODULES_IMAGE_NAME, Arguments::get_java_home(), fileSep, fileSep);
  }

  char path[PATH_MAX];
  struct dirent *dp;

  DIR *dir = opendir("/proc/self/fd");
  while (dp = readdir(dir)) {
    int fd = atoi(dp->d_name);
    if (fd > 2 && fd != dirfd(dir)) {
      int r = readfdlink(fd, path, sizeof(path));
      if (!is_fd_ignored(fd, r != -1 ? path : nullptr)) {
        log_warning(os)("CRaC closing file descriptor %d: %s", fd, path);
        close(fd);
      }
    }
  }
  closedir(dir);
}

void crac::vm_create_start() {
  if (!CRaCCheckpointTo) {
    return;
  }
  close_extra_descriptors();
  _vm_inited_fds.initialize();
}

static bool read_all(int fd, char *dest, size_t n) {
  size_t rd = 0;
  do {
    ssize_t r = ::read(fd, dest + rd, n - rd);
    if (r == 0) {
      return false;
    } else if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    rd += r;
  } while (rd < n);
  return true;
}

bool crac::read_bootid(char *dest) {
  int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY);
  if (fd < 0 || !read_all(fd, dest, UUID_LENGTH)) {
    perror("CRaC: Cannot read system boot ID");
    return false;
  }
  char c;
  if (!read_all(fd, &c, 1) || c != '\n') {
    perror("CRaC: system boot ID does not end with newline");
    return false;
  }
  if (::read(fd, &c, 1) != 0) {
    perror("CRaC: Unexpected data/error reading system boot ID");
    return false;
  }
  if (::close(fd) != 0) {
    perror("CRaC: Cannot close system boot ID file");
  }
  return true;
}
