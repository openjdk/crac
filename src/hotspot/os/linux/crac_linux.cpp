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
#include "attachListener_linux.hpp"
#include "classfile/classLoader.hpp"
#include "jvm.h"
#include "linuxAttachOperation.hpp"
#include "memory/oopFactory.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "perfMemory_linux.hpp"
#include "runtime/arguments.hpp"
#include "runtime/crac.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/vmOperation.hpp"
#include "runtime/vmThread.hpp"
#include "services/attachListener.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/growableArray.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

struct CracFailDep {
  int _type;
  char* _msg;
  CracFailDep(int type, char* msg) :
    _type(type),
    _msg(msg)
  { }
  CracFailDep() :
    _type(JVM_CR_FAIL),
    _msg(NULL)
  { }
};

class CracRestoreParameters : public CHeapObj<mtInternal> {
  char* _raw_content;
  GrowableArray<const char *>* _properties;
  const char* _args;

  struct header {
    jlong _restore_time;
    jlong _restore_counter;
    int _nflags;
    int _nprops;
    int _env_memory_size;
  };

  static bool write_check_error(int fd, const void *buf, int count) {
    int wret = write(fd, buf, count);
    if (wret != count) {
      if (wret < 0) {
        perror("shm error");
      } else {
        fprintf(stderr, "write shm truncated");
      }
      return false;
    }
    return true;
  }

  static int system_props_length(const SystemProperty* props) {
    int len = 0;
    while (props != NULL) {
      ++len;
      props = props->next();
    }
    return len;
  }

  static int env_vars_size(const char* const * env) {
    int len = 0;
    for (; *env; ++env) {
      len += strlen(*env) + 1;
    }
    return len;
  }

 public:
  const char *args() const { return _args; }
  GrowableArray<const char *>* properties() const { return _properties; }

  CracRestoreParameters() :
    _raw_content(NULL),
    _properties(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<const char *>(0, mtInternal)),
    _args(NULL)
  {}

  ~CracRestoreParameters() {
    if (_raw_content) {
      FREE_C_HEAP_ARRAY(char, _raw_content);
    }
    delete _properties;
  }

  static bool write_to(int fd,
      const char* const* flags, int num_flags,
      const SystemProperty* props,
      const char *args,
      jlong restore_time,
      jlong restore_counter) {
    header hdr = {
      restore_time,
      restore_counter,
      num_flags,
      system_props_length(props),
      env_vars_size(environ)
    };

    if (!write_check_error(fd, (void *)&hdr, sizeof(header))) {
      return false;
    }

    for (int i = 0; i < num_flags; ++i) {
      if (!write_check_error(fd, flags[i], strlen(flags[i]) + 1)) {
        return false;
      }
    }

    const SystemProperty* p = props;
    while (p != NULL) {
      char prop[4096];
      int len = snprintf(prop, sizeof(prop), "%s=%s", p->key(), p->value());
      guarantee((0 < len) && ((unsigned)len < sizeof(prop)), "property does not fit temp buffer");
      if (!write_check_error(fd, prop, len+1)) {
        return false;
      }
      p = p->next();
    }

    // Write env vars
    for (char** env = environ; *env; ++env) {
      if (!write_check_error(fd, *env, strlen(*env) + 1)) {
        return false;
      }
    }

    return write_check_error(fd, args, strlen(args)+1); // +1 for null char
  }

  bool read_from(int fd);

};

class VM_Crac: public VM_Operation {
  jarray _fd_arr;
  const bool _dry_run;
  bool _ok;
  GrowableArray<CracFailDep>* _failures;
  CracRestoreParameters _restore_parameters;
  outputStream* _ostream;
  LinuxAttachOperation* _attach_op;

public:
  VM_Crac(jarray fd_arr, jobjectArray obj_arr, bool dry_run, bufferedStream* jcmd_stream) :
    _fd_arr(fd_arr),
    _dry_run(dry_run),
    _ok(false),
    _failures(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<CracFailDep>(0, mtInternal)),
    _restore_parameters(),
    _ostream(jcmd_stream ? jcmd_stream : tty),
    _attach_op(jcmd_stream ? LinuxAttachListener::get_current_op() : NULL)
  { }

  ~VM_Crac() {
    delete _failures;
  }

  GrowableArray<CracFailDep>* failures() { return _failures; }
  bool ok() { return _ok; }
  const char* new_args() { return _restore_parameters.args(); }
  GrowableArray<const char *>* new_properties() { return _restore_parameters.properties(); }
  virtual bool allow_nested_vm_operations() const  { return true; }
  VMOp_Type type() const { return VMOp_VM_Crac; }
  void doit();
  bool read_shm(int shmid);

private:
  bool is_claimed_fd(int fd);
  bool is_socket_from_jcmd(int sock_fd);
  void report_ok_to_jcmd_if_any();
  void print_resources(const char* msg, ...);
  void trace_cr(const char* msg, ...);
};

static const char* _crengine = NULL;
static char* _crengine_arg_str = NULL;
static unsigned int _crengine_argc = 0;
static const char* _crengine_args[32];
static jlong _restore_start_time;
static jlong _restore_start_counter;
static FdsInfo _vm_inited_fds(false);

jlong crac::Linux::restore_start_time() {
  if (!_restore_start_time) {
    return -1;
  }
  return _restore_start_time;
}

jlong crac::Linux::uptime_since_restore() {
  if (!_restore_start_counter) {
    return -1;
  }
  return os::javaTimeNanos() - _restore_start_counter;
}

void VM_Crac::trace_cr(const char* msg, ...) {
  if (CRTrace) {
    va_list ap;
    va_start(ap, msg);
    _ostream->print("CR: ");
    _ostream->vprint_cr(msg, ap);
    va_end(ap);
  }
}

void VM_Crac::print_resources(const char* msg, ...) {
  if (CRPrintResourcesOnCheckpoint) {
    va_list ap;
    va_start(ap, msg);
    _ostream->vprint(msg, ap);
    va_end(ap);
  }
}

void crac::Linux::vm_create_start() {
  if (!CRaCCheckpointTo) {
    return;
  }
  close_extra_descriptors();
  _vm_inited_fds.initialize();
}

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

static int cr_util_path(char* path, int len) {
  os::jvm_path(path, len);
  // path is ".../lib/server/libjvm.so"
  char *after_elem = NULL;
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(path, '/');
    *after_elem = '\0';
  }
  return after_elem - path;
}

static bool compute_crengine() {
  // release possible old copies
  os::free((char *) _crengine); // NULL is allowed
  _crengine = NULL;
  os::free((char *) _crengine_arg_str);
  _crengine_arg_str = NULL;

  if (!CREngine) {
    return true;
  }
  char *exec = os::strdup_check_oom(CREngine);
  char *comma = strchr(exec, ',');
  if (comma != NULL) {
    *comma = '\0';
    _crengine_arg_str = os::strdup_check_oom(comma + 1);
  }
  if (exec[0] == '/') {
    _crengine = exec;
  } else {
    char path[JVM_MAXPATHLEN];
    int pathlen = cr_util_path(path, sizeof(path));
    strcat(path + pathlen, "/");
    strcat(path + pathlen, exec);

    struct stat st;
    if (0 != stat(path, &st)) {
      warning("Could not find %s: %s", path, strerror(errno));
      return false;
    }
    _crengine = os::strdup_check_oom(path);
    // we have read and duplicated args from exec, now we can release
    os::free(exec);
  }
  _crengine_args[0] = _crengine;
  _crengine_argc = 2;

  if (_crengine_arg_str != NULL) {
    char *arg = _crengine_arg_str;
    char *target = _crengine_arg_str;
    bool escaped = false;
    for (char *c = arg; *c != '\0'; ++c) {
      if (_crengine_argc >= ARRAY_SIZE(_crengine_args) - 2) {
        warning("Too many options to CREngine; cannot proceed with these: %s", arg);
        return false;
      }
      if (!escaped) {
        switch(*c) {
        case '\\':
          escaped = true;
          continue; // for
        case ',':
          *target++ = '\0';
          _crengine_args[_crengine_argc++] = arg;
          arg = target;
          continue; // for
        }
      }
      escaped = false;
      *target++ = *c;
    }
    *target = '\0';
    _crengine_args[_crengine_argc++] = arg;
    _crengine_args[_crengine_argc] = NULL;
  }
  return true;
}

static void add_crengine_arg(const char *arg) {
  if (_crengine_argc >= ARRAY_SIZE(_crengine_args) - 1) {
      warning("Too many options to CREngine; cannot add %s", arg);
      return;
  }
  _crengine_args[_crengine_argc++] = arg;
  _crengine_args[_crengine_argc] = NULL;
}

static int call_crengine() {
  if (!_crengine) {
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("cannot fork for crengine");
    return -1;
  }
  if (pid == 0) {
    _crengine_args[1] = "checkpoint";
    add_crengine_arg(CRaCCheckpointTo);
    execv(_crengine, (char * const*)_crengine_args);
    perror("execv CREngine checkpoint");
    exit(1);
  }

  int status;
  int ret;
  do {
    ret = waitpid(pid, &status, 0);
  } while (ret == -1 && errno == EINTR);

  if (ret == -1 || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status) == 0 ? 0 : -1;
}

class CracSHM {
  char _path[128];
public:
  CracSHM(int id) {
    int shmpathlen = snprintf(_path, sizeof(_path), "/crac_%d", id);
    if (shmpathlen < 0 || sizeof(_path) <= (size_t)shmpathlen) {
      fprintf(stderr, "shmpath is too long: %d\n", shmpathlen);
    }
  }

  int open(int mode) {
    int shmfd = shm_open(_path, mode, 0600);
    if (-1 == shmfd) {
      perror("shm_open");
    }
    return shmfd;
  }

  void unlink() {
    shm_unlink(_path);
  }
};

static int checkpoint_restore(int *shmid) {

  int cres = call_crengine();
  if (cres < 0) {
    return JVM_CHECKPOINT_ERROR;
  }

  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, RESTORE_SIGNAL);

  siginfo_t info;
  int sig;
  do {
    sig = sigwaitinfo(&waitmask, &info);
  } while (sig == -1 && errno == EINTR);
  assert(sig == RESTORE_SIGNAL, "got what requested");

  if (CRTraceStartupTime) {
    tty->print_cr("STARTUPTIME " JLONG_FORMAT " restore-native", os::javaTimeNanos());
  }

  if (info.si_code != SI_QUEUE || info.si_int < 0) {
    tty->print("JVM: invalid info for restore provided: %s", info.si_code == SI_QUEUE ? "queued" : "not queued");
    if (info.si_code == SI_QUEUE) {
      tty->print(" code %d", info.si_int);
    }
    tty->cr();
    return JVM_CHECKPOINT_ERROR;
  }

  if (0 < info.si_int) {
    *shmid = info.si_int;
  }

  return JVM_CHECKPOINT_OK;
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

bool VM_Crac::read_shm(int shmid) {
  CracSHM shm(shmid);
  int shmfd = shm.open(O_RDONLY);
  shm.unlink();
  if (shmfd < 0) {
    return false;
  }
  bool ret = _restore_parameters.read_from(shmfd);
  close(shmfd);
  return ret;
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

bool VM_Crac::is_claimed_fd(int fd) {
  typeArrayOop claimed_fds = typeArrayOop(JNIHandles::resolve_non_null(_fd_arr));
  for (int j = 0; j < claimed_fds->length(); ++j) {
    jint cfd = claimed_fds->int_at(j);
    if (fd == cfd) {
      return true;
    }
  }
  return false;
}

void VM_Crac::doit() {

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

  if ((!ok || _dry_run) && CRHeapDumpOnCheckpointException) {
    HeapDumper::dump_heap();
  }

  if (!ok && CRDoThrowCheckpointException) {
    return;
  } else if (_dry_run) {
    _ok = ok;
    return;
  }

  if (!PerfMemoryLinux::checkpoint(CRaCCheckpointTo)) {
    return;
  }

  int shmid = 0;
  if (CRAllowToSkipCheckpoint) {
    trace_cr("Skip Checkpoint");
  } else {
    trace_cr("Checkpoint ...");
    report_ok_to_jcmd_if_any();
    int ret = checkpoint_restore(&shmid);
    if (ret == JVM_CHECKPOINT_ERROR) {
      PerfMemoryLinux::restore();
      return;
    }
  }

  if (shmid <= 0 || !VM_Crac::read_shm(shmid)) {
    _restore_start_time = os::javaTimeMillis();
    _restore_start_counter = os::javaTimeNanos();
  }
  PerfMemoryLinux::restore();

  _ok = true;
}

bool crac::Linux::prepare_checkpoint() {
  struct stat st;

  if (0 == stat(CRaCCheckpointTo, &st)) {
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
      warning("%s: not a directory", CRaCCheckpointTo);
      return false;
    }
  } else {
    if (-1 == mkdir(CRaCCheckpointTo, 0700)) {
      warning("cannot create %s: %s", CRaCCheckpointTo, strerror(errno));
      return false;
    }
    if (-1 == rmdir(CRaCCheckpointTo)) {
      warning("cannot cleanup after check: %s", strerror(errno));
      // not fatal
    }
  }

  if (!compute_crengine()) {
    return false;
  }

  return true;
}

static Handle ret_cr(int ret, Handle new_args, Handle new_props, Handle err_codes, Handle err_msgs, TRAPS) {
  objArrayOop bundleObj = oopFactory::new_objectArray(5, CHECK_NH);
  objArrayHandle bundle(THREAD, bundleObj);
  jvalue jval = { .i = ret };
  oop retObj = java_lang_boxing_object::create(T_INT, &jval, CHECK_NH);
  bundle->obj_at_put(0, retObj);
  bundle->obj_at_put(1, new_args());
  bundle->obj_at_put(2, new_props());
  bundle->obj_at_put(3, err_codes());
  bundle->obj_at_put(4, err_msgs());
  return bundle;
}

/** Checkpoint main entry.
 */
Handle crac::Linux::checkpoint(jarray fd_arr, jobjectArray obj_arr, bool dry_run, jlong jcmd_stream, TRAPS) {
  if (!CRaCCheckpointTo) {
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  if (-1 == mkdir(CRaCCheckpointTo, 0700) && errno != EEXIST) {
    warning("cannot create %s: %s", CRaCCheckpointTo, strerror(errno));
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  Universe::heap()->set_cleanup_unused(true);
  Universe::heap()->collect(GCCause::_full_gc_alot);
  Universe::heap()->set_cleanup_unused(false);

  VM_Crac cr(fd_arr, obj_arr, dry_run, (bufferedStream*)jcmd_stream);
  {
    MutexLocker ml(Heap_lock);
    VMThread::execute(&cr);
  }
  if (cr.ok()) {
    oop new_args = NULL;
    if (cr.new_args()) {
      new_args = java_lang_String::create_oop_from_str(cr.new_args(), CHECK_NH);
    }
    GrowableArray<const char *>* new_properties = cr.new_properties();
    objArrayOop propsObj = oopFactory::new_objArray(vmClasses::String_klass(), new_properties->length(), CHECK_NH);
    objArrayHandle props(THREAD, propsObj);

    for (int i = 0; i < new_properties->length(); i++) {
      oop propObj = java_lang_String::create_oop_from_str(new_properties->at(i), CHECK_NH);
      props->obj_at_put(i, propObj);
    }
    return ret_cr(JVM_CHECKPOINT_OK, Handle(THREAD, new_args), props, Handle(), Handle(), THREAD);
  }

  GrowableArray<CracFailDep>* failures = cr.failures();

  typeArrayOop codesObj = oopFactory::new_intArray(failures->length(), CHECK_NH);
  typeArrayHandle codes(THREAD, codesObj);
  objArrayOop msgsObj = oopFactory::new_objArray(vmClasses::String_klass(), failures->length(), CHECK_NH);
  objArrayHandle msgs(THREAD, msgsObj);

  for (int i = 0; i < failures->length(); ++i) {
    codes->int_at_put(i, failures->at(i)._type);
    oop msgObj = java_lang_String::create_oop_from_str(failures->at(i)._msg, CHECK_NH);
    FREE_C_HEAP_ARRAY(char, failures->at(i)._msg);
    msgs->obj_at_put(i, msgObj);
  }

  return ret_cr(JVM_CHECKPOINT_ERROR, Handle(), Handle(), codes, msgs, THREAD);
}

void crac::Linux::restore() {
  struct stat st;

  jlong restore_time = os::javaTimeMillis();
  jlong restore_counter = os::javaTimeNanos();

  compute_crengine();

  int id = getpid();
  CracSHM shm(id);
  int shmfd = shm.open(O_RDWR | O_CREAT);
  if (0 <= shmfd) {
    if (CracRestoreParameters::write_to(
          shmfd,
          Arguments::jvm_flags_array(), Arguments::num_jvm_flags(),
          Arguments::system_properties(),
          Arguments::java_command() ? Arguments::java_command() : "",
          restore_time,
          restore_counter)) {
      char strid[32];
      snprintf(strid, sizeof(strid), "%d", id);
      setenv("CRAC_NEW_ARGS_ID", strid, true);
    }
    close(shmfd);
  }


  if (_crengine) {
    _crengine_args[1] = "restore";
    add_crengine_arg(CRaCRestoreFrom);
    execv(_crengine, (char * const*) _crengine_args);
    warning("cannot execute \"%s restore ...\" (%s)", _crengine, strerror(errno));
  }
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

void crac::Linux::close_extra_descriptors() {
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

bool CracRestoreParameters::read_from(int fd) {
  struct stat st;
  if (fstat(fd, &st)) {
    perror("fstat (ignoring restore parameters)");
    return false;
  }

  char *contents = NEW_C_HEAP_ARRAY(char, st.st_size, mtInternal);
  if (read(fd, contents, st.st_size) < 0) {
    perror("read (ignoring restore parameters)");
    FREE_C_HEAP_ARRAY(char, contents);
    return false;
  }

  _raw_content = contents;

  // parse the contents to read new system properties and arguments
  header* hdr = (header*)_raw_content;
  char* cursor = _raw_content + sizeof(header);

  ::_restore_start_time = hdr->_restore_time;
  ::_restore_start_counter = hdr->_restore_counter;

  for (int i = 0; i < hdr->_nflags; i++) {
    FormatBuffer<80> err_msg("%s", "");
    JVMFlag::Error result;
    const char *name = cursor;
    if (*cursor == '+' || *cursor == '-') {
      name = cursor + 1;
      result = WriteableFlags::set_flag(name, *cursor == '+' ? "true" : "false",
        JVMFlagOrigin::CRAC_RESTORE, err_msg);
      cursor += strlen(cursor) + 1;
    } else {
      char* eq = strchrnul(cursor, '=');
      if (*eq == '\0') {
        result = JVMFlag::Error::MISSING_VALUE;
        cursor = eq + 1;
      } else {
        *eq = '\0';
        char* value = eq + 1;
        result = WriteableFlags::set_flag(cursor, value, JVMFlagOrigin::CRAC_RESTORE, err_msg);
        cursor = value + strlen(value) + 1;
      }
    }
    guarantee(result == JVMFlag::Error::SUCCESS, "VM Option '%s' cannot be changed: %s",
        name, JVMFlag::flag_error_str(result));
  }

  for (int i = 0; i < hdr->_nprops; i++) {
    assert((cursor + strlen(cursor) <= contents + st.st_size), "property length exceeds shared memory size");
    int idx = _properties->append(cursor);
    int prop_len = strlen(cursor) + 1;
    cursor = cursor + prop_len;
  }

  char* env_mem = NEW_C_HEAP_ARRAY(char, hdr->_env_memory_size, mtArguments); // left this pointer unowned, it is freed when process dies
  memcpy(env_mem, cursor, hdr->_env_memory_size);

  const char* env_end = env_mem + hdr->_env_memory_size;
  while (env_mem < env_end) {
    const size_t s = strlen(env_mem) + 1;
    assert(env_mem + s <= env_end, "env vars exceed memory buffer, maybe ending 0 is lost");
    putenv(env_mem);
    env_mem += s;
  }
  cursor += hdr->_env_memory_size;

  _args = cursor;
  return true;
}
