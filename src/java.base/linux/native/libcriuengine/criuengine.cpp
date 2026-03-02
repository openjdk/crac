/*
 * Copyright (c) 2023, 2026, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crlib/crlib_description.h"
#include "crlib/crlib_restore_data.h"
#include "crlib/crlib_user_data.h"
#include "crcommon.hpp"
#include "hashtable.hpp"
#include "image_constraints.hpp"
#include "image_score.hpp"
#include "user_data.hpp"
#include "jni.h"
#include "jvm.h"

// When adding a new option ensure the proper default value is set for it
// in the configuration struct.
//
// Place more frequently used options first - this will make them faster to find
// in the options hash table.
#define UNCHECKED_OPTIONS(OPT) \
  OPT(image_location, const char*, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "path", "no default", \
    "path to a directory with checkpoint/restore files.") \
  OPT(criu_location, const char*, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "path", "<engine dir>/../lib/criu", \
    "path to the CRIU executable.") \
  OPT(args, const char*, nullptr, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "string", "\"\"", \
    "free space-separated arguments passed directly to the engine executable, e.g. \"--arg1 --arg2 --arg3\".") \

#define CHECKED_OPTIONS(OPT) \
  OPT(keep_running, bool, false, CRLIB_OPTION_FLAG_CHECKPOINT, "true/false", "false", \
    "keep the process running after the checkpoint or kill it.") \
  OPT(direct_map, bool, true, CRLIB_OPTION_FLAG_RESTORE, "true/false", "true", \
    "on restore, map process data directly from saved files. This may speedup the restore " \
    "but the resulting process will not be the same as before the checkpoint.") \
  OPT(legacy_criu, bool, false, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "true/false", "false", \
    "use CRIU options suitable for legacy version (< 4.0).") \
  OPT(print_command, bool, false, CRLIB_OPTION_FLAG_CHECKPOINT | CRLIB_OPTION_FLAG_RESTORE, "true/false", "false", \
    "print CRIU command (for debugging).")

#define CONFIGURE_OPTIONS(OPT) UNCHECKED_OPTIONS(OPT) CHECKED_OPTIONS(OPT)

#define DEFINE_OPT(id, ...) static constexpr char opt_##id[] = #id;
CONFIGURE_OPTIONS(DEFINE_OPT)
#undef DEFINE_OPT
#define ADD_ARR_ELEM(id, ...) opt_##id,
static constexpr const char* configure_options_names[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) nullptr };
#undef ADD_ARR_ELEM
#define ADD_ARR_ELEM(id, ctype, cdef, flags, ...) { opt_##id, static_cast<crlib_conf_option_flag_t>(flags), __VA_ARGS__ },
static constexpr const crlib_conf_option_t configure_options[] = { CONFIGURE_OPTIONS(ADD_ARR_ELEM) {} };
#undef ADD_ARR_ELEM

static char* strdup_checked(const char* src) {
  char* const res = strdup(src);
  if (res == nullptr) {
    LOG("out of memory");
  }
  return res;
}

template <typename T> struct Option {
  T value;
  bool is_default = true;
};

static bool parse_bool(const char* str, Option<bool>* result) {
  if (strcmp(str, "true") == 0) {
    *result = {true, false};
    return true;
  }
  if (strcmp(str, "false") == 0) {
    *result = {false, false};
    return true;
  }
  LOG("expected '%s' to be either 'true' or 'false'", str);
  return false;
}

class ArgsBuilder {
  const char* _args[32];
  const char** _next = _args;
  const char** const _end = _args + ARRAY_SIZE(_args) - 1;
  char* _from_env = nullptr;
  char* _from_opts = nullptr;
  bool _failed = false;

  void fill_args(const char* from, char* copy) {
    char* saveptr = nullptr;
    char* arg = strtok_r(copy, " ", &saveptr);
    while (arg) {
      if (_next < _end) {
        *_next++ = arg;
      } else {
        LOG("Warning: too many arguments for CRIU (dropped '%s' from %s)\n", arg, from);
      }
      arg = strtok_r(NULL, " ", &saveptr);
    }
  }

public:
  ArgsBuilder() {
    memset(_args, 0, sizeof(_args));
  }

  ~ArgsBuilder() {
    free(_from_env);
    free(_from_opts);
  }

  bool failed() const {
    return _failed;
  }

  size_t remaining() const {
    return _end - _next;
  }

  ArgsBuilder& append(const char* arg) {
    if (_next < _end) {
      *_next++ = arg;
    }
    return *this;
  }

  ArgsBuilder& append_all(const char* args) {
    // this method can be called only once
    assert(_from_env == nullptr);
    assert(_from_opts == nullptr);

    const char* criu_opts = getenv("CRAC_CRIU_OPTS");
    if (criu_opts && criu_opts[0]) {
      LOG("CRAC_CRIU_OPTS is deprecated, will be obsoleted in JDK 28 and removed in JDK 29. Use -XX:CRaCEngineOptions=args=...");
      _from_env = strdup_checked(criu_opts);
      if (_from_env == nullptr) {
        _failed = true;
        return *this;
      }
      fill_args("CRAC_CRIU_OPTS", _from_env);
    }
    // applying args later (these have higher priority)
    if (args != nullptr) {
      _from_opts = strdup_checked(args);
      if (_from_opts == nullptr) {
        _failed = true;
        return *this;
      }
      fill_args("args option", _from_opts);
    }
    assert(_next < _args + ARRAY_SIZE(_args));
    return *this;
  }

  char* const* argv() const {
    assert(_next <= _end);
    assert(*_next == nullptr);
    assert(!_failed);
    return const_cast<char* const*>(_args);
  }

  void print() const {
    fprintf(stderr, "%s: Command: ", crcommon_log_prefix());
    for (const char* const* argp = _args; *argp != NULL; ++argp) {
      const char* s = *argp;
      if (argp != _args) {
          fputc(' ', stderr);
      }
      // https://unix.stackexchange.com/a/357932/296319
      if (!strpbrk(s, " \t\n!\"#$&'()*,;<=>?[\\]^`{|}~")) {
          fputs(s, stderr);
          continue;
      }
      fputc('\'', stderr);
      for (; *s; ++s) {
          if (*s != '\'') {
              fputc(*s, stderr);
          } else {
              fputs("'\\''", stderr);
          }
      }
      fputc('\'', stderr);
    }
    fputc('\n', stderr);
  }
};

class criuengine: public crlib_base {
private:
  using configure_func = bool (criuengine::*) (const char* value);
  Hashtable<configure_func> _options {
    configure_options_names, ARRAY_SIZE(configure_options_names) - 1 /* omit nullptr */
  };

#define DEFINE_DEFAULT(id, ctype, cdef, ...) \
  private: Option<ctype> _##id = { cdef, true }; \
  public: inline ctype id() const { return _##id.value; }
  CHECKED_OPTIONS(DEFINE_DEFAULT)
#undef DEFINE_DEFAULT

private:
  char* _image_location = nullptr;
  char* _criu_location = nullptr;
  char* _args = nullptr;
  int _restore_data = 0;

  UserData _user_data;

public:
  criuengine():
    crlib_base("criuengine"),
    _user_data(const_cast<const char**>(&_image_location)) {
    if (!_options.is_initialized()) {
      LOG("out of memory");
      assert(!is_initialized());
      return;
    }
#define PUT_HANDLER(id, ...) _options.put(opt_##id, &criuengine::configure_##id);
    CONFIGURE_OPTIONS(PUT_HANDLER)
#undef PUT_HANDLER
  }

  ~criuengine() {
    free(_image_location);
    free(_criu_location);
    free(_args);
  }

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _common != nullptr && _options.is_initialized(); }

  int restore_data() const { return _restore_data; }

  void require_defaults(crlib_conf_option_flag_t flag, const char* event) const {
#define CHECK_OPT(id, ctype, cdef, flags, ...) \
  if (!_##id.is_default && !((flags) & flag)) { \
    LOG(#id " has no effect on %s", event); \
  }
    CHECKED_OPTIONS(CHECK_OPT)
#undef CHECK_OPT
  }

  bool can_configure(const char* key) const {
    assert(key != nullptr);
    return _options.contains(key);
  }

  bool configure(const char* key, const char* value) {
    assert(key != nullptr && value != nullptr);
    auto* const func = _options.get(key);
    if (func != nullptr) {
      return (this->**func)(value);
    }
    LOG("unknown configure option: %s", key);
    return false;
  }

  bool set_restore_data(const void* data, size_t size) {
    constexpr const size_t supported_size = sizeof(_restore_data);
    if (size > 0 && size != supported_size) {
      LOG("unsupported size of restore data: %zu was requested but only %zu is supported", size, supported_size);
      return false;
    }
    if (size > 0) {
      memcpy(&_restore_data, data, size);
    } else {
      _restore_data = 0;
    }
    return true;
  }

  size_t get_restore_data(void* buf, size_t size) {
    constexpr const size_t available_size = sizeof(_restore_data);
    if (size > 0) {
      memcpy(buf, &_restore_data, size < available_size ? size : available_size);
    }
    return available_size;
  }

  UserData& user_data() {
    return _user_data;
  }

  int checkpoint();
  int restore();
  const char* get_criu();
  bool execute_criu(const ArgsBuilder& args) const;

private:
  bool configure_str(const char* value, char** field) {
    char* copy = strdup_checked(value);
    if (copy == nullptr) {
      return false;
    }
    free(*field);
    *field = copy;
    return true;
  }

  bool configure_image_location(const char* image_location) {
    return configure_str(image_location, &_image_location);
  }

  bool configure_criu_location(const char* criu_location) {
    if (criu_location[0] != '/') {
      LOG("expected absolute path: %s", criu_location);
      return false;
    }
    return configure_str(criu_location, &_criu_location);
  }

  bool configure_keep_running(const char* keep_running_str) {
    return parse_bool(keep_running_str, &_keep_running);
  }

  bool configure_direct_map(const char* direct_map_str) {
    return parse_bool(direct_map_str, &_direct_map);
  }

  bool configure_legacy_criu(const char* legacy) {
    return parse_bool(legacy, &_legacy_criu);
  }

  bool configure_print_command(const char* print) {
    return parse_bool(print, &_print_command);
  }

  bool configure_args(const char* args) {
    return configure_str(args, &_args);
  }
};

RENAME_CRLIB(criuengine);

static crlib_conf_t* create_criuengine() {
  auto* const conf = new(std::nothrow) criuengine();
  if (conf == nullptr) {
    LOG("Cannot create criuengine instance (out of memory)");
    return nullptr;
  } else if (!conf->is_initialized()) {
    delete conf;
    return nullptr;
  }
  return static_cast<crlib_conf_t*>(conf);
}

static void destroy_criuengine(crlib_conf_t* conf) {
  delete static_cast<criuengine*>(conf);
}

static int checkpoint(crlib_conf_t* conf) {
  return conf->checkpoint();
}

static int restore(crlib_conf_t* conf) {
  return conf->restore();
}

static bool can_configure(crlib_conf_t* conf, const char* key) {
  return conf->can_configure(key);
}

static bool configure(crlib_conf_t* conf, const char* key, const char* value) {
  return conf->configure(key, value);
}

static const char* identity(crlib_conf_t* conf) {
  return "criuengine";
}

static const char* description(crlib_conf_t* conf) {
  return "criuengine - CRaC-engine implementing the checkpoint and restore via CRIU";
}

static const char* configuration_doc(crlib_conf_t* conf) {
#define DOC_ITEM(name, ctype, cdef, flags, type, _default, description) \
  "* " #name "=<" type "> (default: " _default ") - " description "\n"
  return CONFIGURE_OPTIONS(DOC_ITEM);
#undef DOC_ITEM
}

static const char* const* configurable_keys(crlib_conf_t* conf) {
  return configure_options_names;
}

static const crlib_conf_option_t* configuration_options(crlib_conf_t* conf) {
   return configure_options;
}

static bool set_restore_data(crlib_conf_t* conf, const void* data, size_t size) {
  return conf->set_restore_data(data, size);
}

static size_t get_restore_data(crlib_conf_t* conf, void* buf, size_t size) {
  return conf->get_restore_data(buf, size);
}

static bool set_user_data(crlib_conf_t* conf, const char* name, const void* data, size_t size) {
  return conf->user_data().set_user_data(name, data, size);
}

static crlib_user_data_storage_t* load_user_data(crlib_conf_t* conf) {
  return conf->user_data().load_user_data();
}

static bool lookup_user_data(crlib_user_data_storage_t* storage, const char* name, const void** data_p, size_t* size_p) {
  return storage->user_data->lookup_user_data(storage, name, data_p, size_p);
}

static void destroy_user_data(crlib_user_data_storage_t* storage) {
  return storage->user_data->destroy_user_data(storage);
}

static const char* check_criu_executable(const char* path, bool quiet) {
  struct stat st;
  if (stat(path, &st)) {
    if (!quiet) {
      if (errno == ENOENT) {
        LOG("CRIU executable does not exist in %s", path);
      } else {
        LOG("Cannot stat %s: %s", path, strerror(errno));
      }
    }
    return nullptr;
  }
  if ((st.st_mode & S_IXUSR) == 0) {
    if (!quiet) {
      LOG("CRIU at %s is not executable", path);
    }
    return nullptr;
  }
  if ((st.st_mode & S_ISUID) == 0 && getuid() != 0) {
    LOG("Warning: CRIU at %s does not have the SUID bit set and you're not a root", path);
  }
  if (st.st_uid != 0 && getuid() != 0) {
    LOG("Warning: CRIU at %s is not owned by the root user and you're not a root", path);
  }
  return path;
}

static char* get_relative_file(const char* rel) {
  Dl_info symbol_info;
  // any function in this file would work
  if (!dladdr(reinterpret_cast<const void*>(::get_relative_file), &symbol_info)) {
    LOG("Failed to get criuengine location: %s", dlerror());
    return nullptr;
  }
  const char* fname = symbol_info.dli_fname;
  const char* last_slash = strrchr(fname, '/');
  if (last_slash == nullptr) {
    LOG("Invalid criuengine location (missing '/'): %s", fname);
    return nullptr;
  }
  size_t rel_size = strlen(rel) + 1;
  char* buf = static_cast<char*>(malloc(last_slash - fname + 1 + rel_size));
  if (buf == nullptr) {
    LOG("Cannot allocate memory for path to %s", rel);
    return nullptr;
  }
  char* end = static_cast<char*>(mempcpy(buf, fname, last_slash - fname + 1));
  memcpy(end, rel, rel_size);
  return buf;
}

const char* criuengine::get_criu() {
  char* criu = _criu_location;
  if (criu == nullptr) {
    criu = getenv("CRAC_CRIU_PATH");
    if (criu != nullptr) {
      LOG("CRAC_CRIU_PATH is deprecated, will be obsoleted in JDK 28 and removed in JDK 29. Use -XX:CRaCEngineOptions=criu_location=...");
    }
  }
  if (criu != nullptr) {
    return check_criu_executable(criu, false);
  }
  const char* const_path = getenv("PATH");
  if (const_path == NULL) {
    LOG("Error: the PATH environment variable is not set, cannot lookup CRIU");
    return nullptr;
  }
  char* path = strdup(const_path);
  if (path == NULL) {
    LOG("Cannot copy PATH; out of memory");
    return nullptr;
  }
  char* save_ptr = nullptr;
  char* prefix = strtok_r(path, ":", &save_ptr);
  while (prefix != nullptr) {
    if (asprintf(&criu, "%s/criu", prefix) < 0) {
      LOG("Cannot allocate string with CRIU location starting with %s", prefix);
    } else {
      free(_criu_location);
      _criu_location = criu;
      if (check_criu_executable(criu, true) != nullptr) {
        return _criu_location;
      }
    }
    prefix = strtok_r(nullptr, ":", &save_ptr);
  }
  LOG("Cannot find CRIU executable on the PATH");
  return nullptr;
}

static int kickjvm(pid_t jvm, int code) {
  union sigval sv = { .sival_int = code };
  if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
    perror("sigqueue");
    return 1;
  }
  return 0;
}

static const char* resolve(const char* rel, char resolved[PATH_MAX]) {
  char* abs = realpath(rel, resolved);
  return abs ? abs : rel;
}

bool criuengine::execute_criu(const ArgsBuilder& args) const {
  pid_t jvm_pid = getpid();
  pid_t child_pid = fork();
  if (child_pid < 0) {
    LOG("Failed to fork: %s", strerror(errno));
    return false;
  }
  if (child_pid) {
    // This is JVM
    if (waitpid(child_pid, NULL, 0) < 0) {
      LOG("Failed to wait for %d: %s", child_pid, strerror(errno));
      return false;
    }
    return true;
  }

  pid_t parent_before = getpid();
  pid_t grandchild_pid = fork();
  if (grandchild_pid < 0) {
    LOG("Failed to fork grandchild: %s", strerror(errno));
    kickjvm(jvm_pid, -1);
  }
  if (grandchild_pid) {
    // intermediate process, terminates immediately
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
    LOG("can't move out of JVM process hierarchy");
    kickjvm(jvm_pid, -1);
    exit(0);
  }

  constexpr const int SUPPRESS_ERROR_IN_PARENT = 77;
  // Technically we could execve right here, but in case of error (bad CRIU args...)
  // the JVM would just wait indefinitely; therefore we'll fork once more and wait
  // for CRIU exit code.
  pid_t child = fork();
  if (!child) {
      execv(args.argv()[0], args.argv());
      LOG("Cannot execute CRIU: %s", strerror(errno));
      args.print();
      exit(SUPPRESS_ERROR_IN_PARENT);
  }

  int status;
  if (child != wait(&status)) {
      LOG("Error waiting for CRIU: %s", strerror(errno));
      args.print();
      kickjvm(jvm_pid, -1);
      exit(0);
  }
  char resolved[PATH_MAX];
  if (!WIFEXITED(status)) {
      LOG("CRIU has not properly exited, waitpid status was %d - check %s/dump4.log", status, resolve(_image_location, resolved));
      args.print();
      kickjvm(jvm_pid, -1);
  } else if (WEXITSTATUS(status)) {
      if (WEXITSTATUS(status) != SUPPRESS_ERROR_IN_PARENT) {
          LOG("CRIU failed with exit code %d - check %s/dump4.log", WEXITSTATUS(status), resolve(_image_location, resolved));
          args.print();
      }
      kickjvm(jvm_pid, -1);
  } else if (keep_running()) {
      kickjvm(jvm_pid, 0);
  }

  exit(0);
}

#define CRAC_FAKE_STDIN  "crac_fake_stdin"
#define CRAC_FAKE_STDOUT "crac_fake_stdout"
#define CRAC_FAKE_STDERR "crac_fake_stderr"

static void maybe_reopen(int fd, int flags) {
  char path[64];
  int mnt_id = -1;
  {
    snprintf(path, sizeof(path), "/proc/thread-self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (!f) {
      LOG("Cannot read %s to check mountinfo", path);
      return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "mnt_id:\t%d", &mnt_id) == 1) {
        break;
      }
    }
    fclose(f);
  }
  if (mnt_id != -1) {
    FILE *f = fopen("/proc/thread-self/mountinfo", "r");
    if (!f) {
      LOG("Cannot read own mountinfo");
      return;
    }
    char line[1024];
    bool is_continue = false;
    while (fgets(line, sizeof(line), f)) {
      if (!is_continue && atoi(line) == mnt_id) {
        // mnt_id found, nothing needs to be done
        fclose(f);
        return;
      }
      is_continue = (line[strlen(line) - 1] != '\n');
    }
    fclose(f);
  }
  snprintf(path, sizeof(path), "/proc/thread-self/fd/%d", fd);
  char target[PATH_MAX];
  ssize_t len = readlink(path, target, sizeof(target));
  if (len < 0) {
    LOG("Cannot readlink %s: %s", path, strerror(errno));
    return;
  }
  target[len] = '\0'; // readlink does not append terminating char
  static const char pipe[] = "pipe:";
  if (!strncmp(pipe, target, sizeof(pipe) - 1)) {
    // cannot reopen a pipe but CRIU will handle this OK
    return;
  }
  int new_fd = open(target, flags);
  if (new_fd < 0) {
    LOG("Cannot reopen %s: %s", target, strerror(errno));
  } else if (dup2(new_fd, fd) != fd) {
    LOG("Cannot dup2 %d (%s) -> %d: %s", new_fd, target, fd, strerror(errno));
  } else {
    close(new_fd);
  }
}

int criuengine::checkpoint() {
  const char* criu = get_criu();
  if (criu == nullptr) {
    return -1;
  }
  if (_image_location == nullptr) {
    LOG("%s must be set before checkpoint", opt_image_location);
    return -1;
  }
  require_defaults(CRLIB_OPTION_FLAG_CHECKPOINT, "checkpoint");

  if (!image_constraints_persist(common(), _image_location) ||
      !image_score_persist(common(), _image_location)) {
    return -1;
  }
  // We will reset scores now; scores can be retained or reset higher on the Java level.
  // Before another checkpoint all the scores will be recorded again; we won't keep
  // anything here to not write down any outdated value.
  image_score_reset(common());

  char jvmpidchar[32];
  snprintf(jvmpidchar, sizeof(jvmpidchar), "%d", getpid());

  ArgsBuilder args;
  args
    .append(criu).append("dump")
    .append("-t").append(jvmpidchar)
    // -D without -W makes criu cd to image dir for logs
    .append("-D").append(_image_location)
    // will be overwritten with any later log-related args
    .append("-v4").append("-o").append("dump4.log")
    .append("--shell-job");

  if (keep_running()) {
    args.append("-R");
  }
  if (!legacy_criu()) {
    args.append("--unprivileged");
  }

  args.append_all(_args);
  if (args.failed()) {
    return -1;
  }

  if (print_command()) {
    args.print();
  }
  fflush(stderr);

  // With upstream CRIU the restored process does not inherit FDs 0-2 if the TTY
  // is not attached. We will create some fake FDs and let CRIU replace them;
  // we will use those if replaced after restore or just close when not (or when kept running).
  int fake_fds[3] = { -1, -1, -1 };
  if (!legacy_criu()) {
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
    fake_fds[0] = syscall(SYS_memfd_create, CRAC_FAKE_STDIN, MFD_CLOEXEC);
    fake_fds[1] = syscall(SYS_memfd_create, CRAC_FAKE_STDOUT, MFD_CLOEXEC);
    fake_fds[2] = syscall(SYS_memfd_create, CRAC_FAKE_STDERR, MFD_CLOEXEC);
    if (fake_fds[0] < 0 || fake_fds[1] < 0 || fake_fds[2] < 0) {
      LOG("Cannot create fake FDs for non-shell jobs: %s", strerror(errno));
    }
    // Also sometimes (in containers) the mnt_id in /proc/<pid>/fdinfo/X and /proc/<pid>/mntinfo
    // does not match; CRIU won't handle that if this is not a tty. Let's try to detect and reopen
    maybe_reopen(0, O_RDONLY);
    maybe_reopen(1, O_WRONLY);
    maybe_reopen(2, O_WRONLY);

    // When executed without a TTY, CRIU won't use --shell-job; however in this situation
    // it won't restore with --shell-job and restore will get stuck when synchronizing
    // a helper process created to establish SID. Becoming own session leader here prevents
    // this (though it might have some other effects).
    if (getsid(0) != getpid()) {
      setpgid(0, setsid());
    }
  }

  if (!execute_criu(args)) {
    for (size_t i = 0; i < ARRAY_SIZE(fake_fds); ++i) {
      close(fake_fds[i]);
    }
    return -1;
  }

  siginfo_t info;
  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, RESTORE_SIGNAL);

  int sig;
  do {
    sig = sigwaitinfo(&waitmask, &info);
  } while (sig == -1 && errno == EINTR);

  for (unsigned i = 0; i < ARRAY_SIZE(fake_fds); ++i) {
    if (fake_fds[i] < 0) {
      continue;
    } else if (fcntl(fake_fds[i], F_GET_SEALS) >= 0) {
      // this is memfd => not replaced, just close
    } else if (dup2(fake_fds[i], i) < -1) {
      // you probably wouldn't see this message, though
      LOG("Failed to dup2(%d, %d): %s", fake_fds[i], i, strerror(errno));
    }
    close(fake_fds[i]);
  }

  if (info.si_code != SI_QUEUE) {
    return -1;
  }
  {
#ifndef NDEBUG
    const bool ok =
#endif // NDEBUG
    set_restore_data(&info.si_int, sizeof(info.si_int));
    assert(ok);
  }

  return 0;
}

int criuengine::restore() {
  const char* criu = get_criu();
  if (criu == nullptr) {
    return -1;
  }
  if (_image_location == nullptr) {
    LOG("%s must be set before restore", opt_image_location);
    return -1;
  }
  require_defaults(CRLIB_OPTION_FLAG_RESTORE, "restore");

  if (!image_constraints_validate(common(), _image_location)) {
    return -1;
  }

  char restore_data_str[32];
  if (snprintf(restore_data_str, sizeof(restore_data_str), "%i", restore_data()) >
      static_cast<int>(sizeof(restore_data_str)) - 1) {
    LOG("snprintf restore data: %s", strerror(errno));
    return -1;
  }

  // We need to use ../lib for static build
  char* criuhelper = get_relative_file("../lib/criuhelper");
  if (criuhelper == nullptr) {
    return -1;
  }
  auto free_criuhelper = defer([&] { free(criuhelper); });

  ArgsBuilder args;
  args
    .append(criu)
    .append("restore")
    .append("-W").append(".")
    .append("--action-script").append(criuhelper)
    .append("-D").append(_image_location)
    // XSAVE is not needed when the snapshot is being made / restored
    .append("--cpu-cap=none")
     // errors-only by default, can be overwritten through -XX:CRaCEngineOptions=args=-v4
    .append("-v1");

  if (legacy_criu()) {
    // always works with legacy CRIU
    args.append("--shell-job");
  } else {
    args.append("--unprivileged");
    // When tty-info is not present we can't use --shell-job and must need to inherit FDs manually
    char tty_info[PATH_MAX];
    if ((size_t) snprintf(tty_info, sizeof(tty_info), "%s/tty-info.img", _image_location) >= sizeof(tty_info)) {
      LOG("Cannot form path to tty-info.img");
      return -1;
    }
    struct stat st;
    if (stat(tty_info, &st) && errno == ENOENT) {
      args.append("--inherit-fd").append("fd[0]:/memfd:" CRAC_FAKE_STDIN);
      args.append("--inherit-fd").append("fd[1]:/memfd:" CRAC_FAKE_STDOUT);
      args.append("--inherit-fd").append("fd[2]:/memfd:" CRAC_FAKE_STDERR);
    } else {
      args.append("--shell-job");
    }
  }
  if (!direct_map()) {
    if (!legacy_criu()) {
      LOG("Warning: direct mapping of image might not be supported");
    }
    args.append("--no-mmap-page-image");
  }

  args.append_all(_args);
  if (args.failed()) {
    return -1;
  }
  if (args.remaining() < 4) {
    LOG("Too many arguments to CRIU");
    args.print();
    return -1;
  }
  args.append("--exec-cmd").append("--").append(criuhelper).append("restorewait");

  if (setenv("CRAC_NEW_ARGS_ID", restore_data_str, 1)) {
    LOG("Cannot set CRAC_NEW_ARGS_ID: %s", strerror(errno));
    return -1;
  }

  if (print_command()) {
    args.print();
  }
  fflush(stderr);
  execve(criu, args.argv(), environ);

  LOG("Cannot execute CRIU: %s", strerror(errno));
  args.print();
  return -1;
}

static crlib_extension_t* const* supported_extensions(crlib_conf_t* conf);

static crlib_description_t description_extension = {
  {
    CRLIB_EXTENSION_DESCRIPTION_NAME,
    sizeof(description_extension)
  },
  identity,
  description,
  configuration_doc,
  configurable_keys,
  supported_extensions,
  configuration_options,
};

static crlib_restore_data_t restore_data_extension = {
  {
    CRLIB_EXTENSION_RESTORE_DATA_NAME,
    sizeof(restore_data_extension)
  },
  set_restore_data,
  get_restore_data,
};

static crlib_user_data_t user_data_extension = {
  {
    CRLIB_EXTENSION_USER_DATA_NAME,
    sizeof(user_data_extension)
  },
  set_user_data,
  load_user_data,
  lookup_user_data,
  destroy_user_data,
};

static const crlib_extension_t* extensions[] = {
  &restore_data_extension.header,
  &image_constraints_extension.header,
  &image_score_extension.header,
  &user_data_extension.header,
  &description_extension.header,
  nullptr
};

static const crlib_extension_t* get_extension(const char* name, size_t size) {
  return find_extension(extensions, name, size);
}

static crlib_extension_t* const* supported_extensions(crlib_conf_t* conf) {
  return extensions;
}

static crlib_api_t api = {
  create_criuengine,
  destroy_criuengine,
  checkpoint,
  restore,
  can_configure,
  configure,
  get_extension,
};

extern "C" {

JNIEXPORT crlib_api_t* CRLIB_API_MAYBE_STATIC(int api_version, size_t api_size) {
  if (api_version != CRLIB_API_VERSION) {
    return nullptr;
  }
  if (sizeof(crlib_api_t) < api_size) {
    return nullptr;
  }
  return &api;
}

}
