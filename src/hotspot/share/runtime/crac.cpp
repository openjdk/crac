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

#include "precompiled.hpp"

#include "classfile/classLoader.hpp"
#include "jvm.h"
#include "memory/oopFactory.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/crac.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/threads.hpp"
#include "runtime/vm_version.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"

static const char* _crengine = NULL;
static char* _crengine_arg_str = NULL;
static unsigned int _crengine_argc = 0;
static const char* _crengine_args[32];
static jlong _restore_start_time;
static jlong _restore_start_nanos;

// Timestamps recorded before checkpoint
jlong crac::checkpoint_millis;
jlong crac::checkpoint_nanos;
char crac::checkpoint_bootid[UUID_LENGTH];
// Value based on wall clock time difference that will guarantee monotonic
// System.nanoTime() close to actual wall-clock time difference.
jlong crac::javaTimeNanos_offset = 0;

jlong crac::restore_start_time() {
  if (!_restore_start_time) {
    return -1;
  }
  return _restore_start_time;
}

jlong crac::uptime_since_restore() {
  if (!_restore_start_nanos) {
    return -1;
  }
  return os::javaTimeNanos() - _restore_start_nanos;
}

void VM_Crac::trace_cr(const char* msg, ...) {
  if (CRTrace) {
    va_list ap;
    va_start(ap, msg);
    _ostream->print("CR: ");
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    _ostream->vprint_cr(msg, ap);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    va_end(ap);
  }
}

void VM_Crac::print_resources(const char* msg, ...) {
  if (CRPrintResourcesOnCheckpoint) {
    va_list ap;
    va_start(ap, msg);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    _ostream->vprint(msg, ap);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    va_end(ap);
  }
}

#if defined(__APPLE__) || defined(_WINDOWS)
static char * strchrnul(char * str, char c) {
  for (; *str && c != *str; ++str) {}
  return str;
}
#endif //__APPLE__ || _WINDOWS

static size_t cr_util_path(char* path, int len) {
  os::jvm_path(path, len);
  // path is ".../lib/server/libjvm.so", or "...\bin\server\libjvm.dll"
  assert(1 == strlen(os::file_separator()), "file separator must be a single-char, not a string");
  char *after_elem = NULL;
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(path, *os::file_separator());
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
  if (os::is_path_absolute(exec)) {
    _crengine = exec;
  } else {
    char path[JVM_MAXPATHLEN];
    size_t pathlen = cr_util_path(path, sizeof(path));
    strcat(path + pathlen, os::file_separator());
    strcat(path + pathlen, exec);

    struct stat st;
    if (0 != os::stat(path, &st)) {
      warning("Could not find %s: %s", path, os::strerror(errno));
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
  _crengine_args[1] = "checkpoint";
  add_crengine_arg(CRaCCheckpointTo);
  return os::exec_child_process_and_wait(_crengine, _crengine_args);
}

static int checkpoint_restore(int *shmid) {
  crac::record_time_before_checkpoint();

  int cres = call_crengine();
  if (cres < 0) {
    tty->print_cr("CRaC error executing: %s\n", _crengine);
    return JVM_CHECKPOINT_ERROR;
  }

#ifdef LINUX
  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, RESTORE_SIGNAL);

  siginfo_t info;
  int sig;
  do {
    sig = sigwaitinfo(&waitmask, &info);
  } while (sig == -1 && errno == EINTR);
  assert(sig == RESTORE_SIGNAL, "got what requested");

  if (CRaCCPUCountInit) {
    os::Linux::initialize_cpu_count();
  }
#else
  // TODO add sync processing
#endif //LINUX

  crac::update_javaTimeNanos_offset();

  if (CRTraceStartupTime) {
    tty->print_cr("STARTUPTIME " JLONG_FORMAT " restore-native", os::javaTimeNanos());
  }

#ifdef LINUX
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
#else
  *shmid = 0;
#endif //LINUX
  return JVM_CHECKPOINT_OK;
}

bool VM_Crac::read_shm(int shmid) {
  CracSHM shm(shmid);
  int shmfd = shm.open(O_RDONLY);
  shm.unlink();
  if (shmfd < 0) {
    return false;
  }
  // deserialize parameters from shared memory
  bool ret = _restore_parameters.deserialize(shmfd);
  if (!ret) {
    fprintf(stderr, "CracRestoreParameters: deserialization failed!\n");
  }
  close(shmfd);

  // set _restore_start_time and _restore_start_nanos from restored parameters
  ::_restore_start_time = _restore_parameters.restore_time;
  ::_restore_start_nanos = _restore_parameters.restore_nanos;

  // update JVM flags from restored parameters
  for (const char* flag : _restore_parameters.flags) {
    FormatBuffer<80> err_msg("%s", "");
    JVMFlag::Error result;
    if (*flag == '+' || *flag == '-') {
      result = WriteableFlags::set_flag(
        flag + 1,
        *flag == '+' ? "true" : "false",
        JVMFlagOrigin::CRAC_RESTORE,
        err_msg
      );
      guarantee(
        result == JVMFlag::Error::SUCCESS,
        "VM Option '%s' cannot be changed: %d",
        flag + 1,
        result
      );
    } else {
      // copy flag to a mutable buffer
      char buf[4096];
      strncpy(buf, flag, sizeof(buf));
      buf[4095] = '\0';
      // replace '=' with '\0' if found
      char* eq = strchr(buf, '=');
      if (eq == NULL) {
        result = JVMFlag::Error::MISSING_VALUE;
      } else {
        *eq = '\0';
        char* name = buf;
        char* value = eq + 1;
        result = WriteableFlags::set_flag(name, value, JVMFlagOrigin::CRAC_RESTORE, err_msg);
      }
      guarantee(
        result == JVMFlag::Error::SUCCESS,
        "VM Option '%s' cannot be changed: %d",
        buf,
        result
      );
    }
  }

  // update environment variables from restored parameters
  for (const char* env : _restore_parameters.envs) {
    // putenv() expects char*, but doesn't actually modify its argument,
    // therefore using const_cast is fine.
    // setenv() might be safer though.
    putenv(const_cast<char*>(env));
  }

  return ret;
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

class WakeupClosure: public ThreadClosure {
  void do_thread(Thread* thread) {
    JavaThread *jt = JavaThread::cast(thread);
    jt->wakeup_sleep();
    jt->parker()->unpark();
    jt->_ParkEvent->unpark();
  }
};

static void wakeup_threads_in_timedwait() {
  WakeupClosure wc;
  Threads::java_threads_do(&wc);

  MonitorLocker ml(PeriodicTask_lock, Mutex::_no_safepoint_check_flag);
  WatcherThread::watcher_thread()->unpark();
}

void VM_Crac::doit() {
  // dry-run fails checkpoint
  bool ok = true;

  if (!check_fds()) {
    ok = false;
  }

  if ((!ok || _dry_run) && CRHeapDumpOnCheckpointException) {
    HeapDumper::dump_heap();
  }

  if (!ok && CRPauseOnCheckpointError) {
    os::message_box("Checkpoint failed", "Errors were found during checkpoint.");
  }

  if (!ok && CRDoThrowCheckpointException) {
    return;
  } else if (_dry_run) {
    _ok = ok;
    return;
  }

  if (!memory_checkpoint()) {
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
      memory_restore();
      return;
    }
  }

  VM_Version::crac_restore();

  if (shmid <= 0 || !VM_Crac::read_shm(shmid)) {
    _restore_start_time = os::javaTimeMillis();
    _restore_start_nanos = os::javaTimeNanos();
  } else {
    _restore_start_nanos += crac::monotonic_time_offset();
  }
  memory_restore();

  wakeup_threads_in_timedwait();

  _ok = true;
}

bool crac::prepare_checkpoint() {
  struct stat st;

  if (0 == os::stat(CRaCCheckpointTo, &st)) {
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
      warning("%s: not a directory", CRaCCheckpointTo);
      return false;
    }
  } else {
    if (-1 == os::mkdir(CRaCCheckpointTo)) {
      warning("cannot create %s: %s", CRaCCheckpointTo, os::strerror(errno));
      return false;
    }
    if (-1 == os::rmdir(CRaCCheckpointTo)) {
      warning("cannot cleanup after check: %s", os::strerror(errno));
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
  jvalue jval;
  jval.i = ret;
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
Handle crac::checkpoint(jarray fd_arr, jobjectArray obj_arr, bool dry_run, jlong jcmd_stream, TRAPS) {
  if (!CRaCCheckpointTo) {
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  if (-1 == os::mkdir(CRaCCheckpointTo) && errno != EEXIST) {
    warning("cannot create %s: %s", CRaCCheckpointTo, os::strerror(errno));
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

// Write a jlong to fd.
// On error, return false.
static bool write_jlong(int fd, jlong value) {
  const size_t JLONG_SIZE = sizeof(jlong);

  // serialize jlong with reinterpret_cast
  int ret = write(fd, reinterpret_cast<char*>(&value), JLONG_SIZE);
  if (ret != JLONG_SIZE) {
    if (ret < 0) {
      perror("write_jlong error");
    } else {
      fprintf(stderr, "write_jlong truncated");
    }
    return false;
  }
  return true;
}

// Read a jlong from fd into value, which should not be NULL.
// On error, return false.
static bool read_jlong(int fd, jlong* value) {
  const size_t JLONG_SIZE = sizeof(jlong);
  guarantee(value != NULL, "read_jlong: value is NULL");

  // deserialize jlong with reinterpret_cast
  int ret = read(fd, reinterpret_cast<char*>(value), JLONG_SIZE);
  if (ret != JLONG_SIZE) {
    if (ret < 0) {
      perror("read_jlong error");
    } else {
      fprintf(stderr, "read_jlong truncated");
    }
    return false;
  }
  return true;
}

// Write a GrowableArray to fd.
// On error, return false.
static bool write_growable_array(int fd, GrowableArray<const char *>* array) {
  const size_t JLONG_SIZE = sizeof(jlong);
  guarantee(array != NULL, "write_growable_array: array is NULL");

  // write array length as jlong
  if (!write_jlong(fd, array->length())) {
    return false;
  }

  // write the content (including trailing NULL bytes)
  for (auto s : *array) {
    ssize_t len = strlen(s);
    if (write(fd, s, len + 1) != len + 1) {
      return false;
    }
  }
  return true;
}

// Append a GrowableArray from fd to array, which should be initialized.
// On error, return false.
static bool read_growable_array(int fd, GrowableArray<const char *>* array) {
  const size_t JLONG_SIZE = sizeof(jlong);
  guarantee(array != NULL, "read_growable_array: array is NULL");

  // read array length as jlong
  jlong len = 0;
  if (!read_jlong(fd, &len)) {
    return false;
  }

  // read the content
  for (int i = 0; i < len; i++) {
    // read the string byte-at-a-time, because we don't know where the NULL byte is
    // calling read() in a loop like this may impact performance
    char buf[4096] = { 0 };
    for (int j = 0; j < 4096; j++) {
      if (read(fd, &buf[j], 1) != 1) {
        return false;
      }
      if (buf[j] == '\0') {
        break;
      }
    }
    // copy the string onto the heap
    size_t size = strlen(buf) + 1;
    char* heap_buf = NEW_C_HEAP_ARRAY(char, size, mtArguments);
    strncpy(heap_buf, buf, size);
    array->append(heap_buf);
  }
  return true;
}

CracRestoreParameters::CracRestoreParameters() :
  restore_time(0),
  restore_nanos(0),
  flags(GrowableArray<const char *>(0, mtInternal)),
  properties(GrowableArray<const char *>(0, mtInternal)),
  args(GrowableArray<const char *>(0, mtInternal)),
  envs(GrowableArray<const char *>(0, mtInternal)) {}

bool CracRestoreParameters::serialize(int fd) {
  if (!write_jlong(fd, restore_time)) return false;
  if (!write_jlong(fd, restore_nanos)) return false;
  if (!write_growable_array(fd, &flags)) return false;
  if (!write_growable_array(fd, &properties)) return false;
  if (!write_growable_array(fd, &args)) return false;
  if (!write_growable_array(fd, &envs)) return false;
  return true;
}

bool CracRestoreParameters::deserialize(int fd) {
  if (!read_jlong(fd, &restore_time)) return false;
  if (!read_jlong(fd, &restore_nanos)) return false;
  if (!read_growable_array(fd, &flags)) return false;
  if (!read_growable_array(fd, &properties)) return false;
  if (!read_growable_array(fd, &args)) return false;
  if (!read_growable_array(fd, &envs)) return false;
  return true;
}

void crac::restore() {
  jlong restore_time = os::javaTimeMillis();
  jlong restore_nanos = os::javaTimeNanos();

  compute_crengine();

  const int id = os::current_process_id();

  CracSHM shm(id);
  int shmfd = shm.open(O_RDWR | O_CREAT);
  if (0 <= shmfd) {
    // passes the context to VM via shared memory as a serialized data structure
    CracRestoreParameters params;

    // record current time
    params.restore_time = restore_time;
    params.restore_nanos = restore_nanos;

    // record JVM flags
    for (int i = 0; i < Arguments::num_jvm_flags(); i++) {
      params.flags.append(Arguments::jvm_flags_array()[i]);
    }

    // record JVM properties
    for (auto p = Arguments::system_properties(); p != NULL; p = p->next()) {
      // allocate memory to store properties on the heap
      // the memory will be freed when the process exits
      size_t len = strlen(p->key()) + strlen(p->value()) + 1;
      char* buf = NEW_C_HEAP_ARRAY(char, len, mtArguments);
      snprintf(buf, len, "%s=%s", p->key(), p->value());
      params.properties.append(buf);
    }

    // record command line arguments
    // TODO: pass argc/argv from JavaMain to here
    params.args.append(Arguments::java_command() ? Arguments::java_command() : "");

    // record environment variables
    for (char** env = os::get_environ(); *env != NULL; ++env) {
      params.envs.append(*env);
    }

    // serialize parameters into shared memory
    if (params.serialize(shmfd)) {
      char strid[32];
      snprintf(strid, sizeof(strid), "%d", id);
      LINUX_ONLY(setenv("CRAC_NEW_ARGS_ID", strid, true));
    } else {
      fprintf(stderr, "CracRestoreParameters: serialization failed!\n");
    }
    close(shmfd);
  }

  if (_crengine) {
    _crengine_args[1] = "restore";
    add_crengine_arg(CRaCRestoreFrom);
    os::execv(_crengine, _crengine_args);
    warning("cannot execute \"%s restore ...\" (%s)", _crengine, os::strerror(errno));
  }
}

void crac::record_time_before_checkpoint() {
  checkpoint_millis = os::javaTimeMillis();
  checkpoint_nanos = os::javaTimeNanos();
  memset(checkpoint_bootid, 0, UUID_LENGTH);
  read_bootid(checkpoint_bootid);
}

void crac::update_javaTimeNanos_offset() {
  char buf[UUID_LENGTH];
  // We will change the nanotime offset only if this is not the same boot
  // to prevent reducing the accuracy of System.nanoTime() unnecessarily.
  // It is possible that in a real-world case the boot_id does not change
  // (containers keep the boot_id) - but the monotonic time changes. We will
  // only guarantee that the nanotime does not go backwards in that case but
  // won't offset the time based on wall-clock time as this change in monotonic
  // time is likely intentional.
  if (!read_bootid(buf) || memcmp(buf, checkpoint_bootid, UUID_LENGTH) != 0) {
    assert(checkpoint_millis >= 0, "Restore without a checkpoint?");
    long diff_millis = os::javaTimeMillis() - checkpoint_millis;
    // If the wall clock has gone backwards we won't add it to the offset
    if (diff_millis < 0) {
      diff_millis = 0;
    }
    // javaTimeNanos() call on the second line below uses the *_offset, so we will zero
    // it to make the call return true monotonic time rather than the adjusted value.
    javaTimeNanos_offset = 0;
    javaTimeNanos_offset = checkpoint_nanos - os::javaTimeNanos() + diff_millis * 1000000L;
  } else {
    // ensure monotonicity even if this looks like the same boot
    jlong diff = os::javaTimeNanos() - checkpoint_nanos;
    if (diff < 0) {
      javaTimeNanos_offset -= diff;
    }
  }
}
