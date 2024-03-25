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
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "jni.h"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/allocation.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/symbolHandle.hpp"
#include "os.inline.hpp"
#include "runtime/crac.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/cracHeapRestorer.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/cracStackDumper.hpp"
#include "runtime/cracThreadRestorer.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/handles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/debug.hpp"
#include "utilities/decoder.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/macros.hpp"

// Filenames used by the portable mode
static constexpr char PMODE_HEAP_DUMP_FILENAME[] = "heap.hprof";
static constexpr char PMODE_STACK_DUMP_FILENAME[] = "stacks.bin";
static constexpr char PMODE_CLASS_DUMP_FILENAME[] = "classes.bin";

static const char* _crengine = NULL;
static char* _crengine_arg_str = NULL;
static unsigned int _crengine_argc = 0;
static const char* _crengine_args[32];
static jlong _restore_start_time;
static jlong _restore_start_nanos;

// Used by portable restore
ParsedCracStackDump *crac::_stack_dump = nullptr;

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

bool crac::is_portable_mode() {
  return CREngine == nullptr;
}

// Checkpoint in portable mode.
static VM_Crac::Outcome checkpoint_portable() {
#if INCLUDE_SERVICES // HeapDumper is a service
  char path[JVM_MAXPATHLEN];

  // Dump thread stacks
  os::snprintf_checked(path, sizeof(path), "%s%s%s",
                       CRaCCheckpointTo, os::file_separator(), PMODE_STACK_DUMP_FILENAME);
  {
    const CracStackDumper::Result res = CracStackDumper::dump(path);
    switch (res.code()) {
      case CracStackDumper::Result::Code::OK: break;
      case CracStackDumper::Result::Code::IO_ERROR: {
        warning("Cannot dump thread stacks into %s: %s", path, res.io_error_msg());
        if (remove(path) != 0) warning("Cannot remove %s: %s", path, os::strerror(errno));
        return VM_Crac::Outcome::FAIL; // User action required
      }
      case CracStackDumper::Result::Code::NON_JAVA_IN_MID: {
        ResourceMark rm;
        warning("Cannot checkpoint now: thread %s has Java frames interleaved with native frames",
                res.problematic_thread()->name());
        if (remove(path) != 0) warning("Cannot remove %s: %s", path, os::strerror(errno));
        return VM_Crac::Outcome::FAIL; // It'll probably take too long to wait until all such frames are gone
      }
      case CracStackDumper::Result::Code::NON_JAVA_ON_TOP: {
        ResourceMark rm;
        warning("Cannot checkpoint now: thread %s is executing native code",
                res.problematic_thread()->name());
        if (remove(path) != 0) warning("Cannot remove %s: %s", path, os::strerror(errno));
        return VM_Crac::Outcome::RETRY; // Hoping the thread will get out of non-Java code shortly
      }
    }
  }

  // Dump classes
  os::snprintf_checked(path, sizeof(path), "%s%s%s",
                       CRaCCheckpointTo, os::file_separator(), PMODE_CLASS_DUMP_FILENAME);
  {
    const char* err = CracClassDumper::dump(path, false /* Don't overwrite */);
    if (err != nullptr) {
      warning("Cannot dump classes into %s: %s", path, err);
      return VM_Crac::Outcome::FAIL; // User action required
    }
  }

  // Dump heap
  os::snprintf_checked(path, sizeof(path), "%s%s%s",
                       CRaCCheckpointTo, os::file_separator(), PMODE_HEAP_DUMP_FILENAME);
  {
    HeapDumper dumper(false, // No GC: it's already been performed by crac::checkpoint()
                      true); // Include all j.l.Class objects and injected fields
    if (dumper.dump(path,
                    nullptr,  // No additional output
                    -1,       // No compression, TODO: enable this when the parser supports it
                    false,    // Don't overwrite
                    HeapDumper::default_num_of_dump_threads()) != 0) {
      ResourceMark rm;
      warning("Cannot dump heap into %s: %s", path, dumper.error_as_C_string());
      return VM_Crac::Outcome::FAIL; // User action required
    }
  }

  return VM_Crac::Outcome::OK;
#else  // INCLUDE_SERVICES
  warning("This VM cannot create checkpoints in portable mode: it is compiled without \"services\" feature");
  return VM_Crac::Outcome::FAIL;
#endif  // INCLUDE_SERVICES
}

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
  assert(!crac::is_portable_mode(), "Portable mode requested, should not call this");

  // release possible old copies
  os::free((char *) _crengine); // NULL is allowed
  _crengine = NULL;
  os::free((char *) _crengine_arg_str);
  _crengine_arg_str = NULL;

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
  bool ret = _restore_parameters.read_from(shmfd);
  close(shmfd);
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

// It requires Threads_lock to be held so it is being run as a part of VM_Operation.
static void wakeup_threads_in_timedwait_vm() {
  WakeupClosure wc;
  Threads::java_threads_do(&wc);
}

// Run it after VM_Operation as it holds Threads_lock which would cause:
// Attempting to acquire lock PeriodicTask_lock/safepoint out of order with lock Threads_lock/safepoint-1 -- possible deadlock
static void wakeup_threads_in_timedwait() {
  MonitorLocker ml(PeriodicTask_lock, Mutex::_safepoint_check_flag);
  WatcherThread::watcher_thread()->unpark();
}

void VM_Crac::doit() {
  // Clear the state (partially: JCMD connection might be gone) if trying again
  if (_outcome == Outcome::RETRY) {
    _outcome = Outcome::FAIL;
    _failures->clear_and_deallocate();
    _restore_parameters.clear();
  }

  // dry-run fails checkpoint
  bool ok = true;

  Decoder::before_checkpoint();
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
    _outcome = ok ? Outcome::OK : Outcome::FAIL;
    return;
  }

  if (!crac::is_portable_mode() && !memory_checkpoint()) {
    return;
  }

  int shmid = 0;
  Outcome outcome = Outcome::OK;
  if (CRAllowToSkipCheckpoint) {
    trace_cr("Skip Checkpoint");
  } else {
    trace_cr("Checkpoint ...");
    report_ok_to_jcmd_if_any();
    if (crac::is_portable_mode()) {
      outcome = checkpoint_portable();
    } else if (checkpoint_restore(&shmid) == JVM_CHECKPOINT_ERROR) {
      memory_restore();
      return;
    }
  }

  // It needs to check CPU features before any other code (such as VM_Crac::read_shm) depends on them.
  VM_Version::crac_restore();

  if (shmid <= 0 || !VM_Crac::read_shm(shmid)) {
    _restore_start_time = os::javaTimeMillis();
    _restore_start_nanos = os::javaTimeNanos();
  } else {
    _restore_start_nanos += crac::monotonic_time_offset();
  }

  if (CRaCResetStartTime) {
    crac::initialize_time_counters();
  }

  // VM_Crac::read_shm needs to be already called to read RESTORE_SETTABLE parameters.
  VM_Version::crac_restore_finalize();

  memory_restore();

  wakeup_threads_in_timedwait_vm();

  _outcome = outcome;
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

  if (!is_portable_mode() && !compute_crengine()) {
    return false;
  }

  return true;
}

Handle crac::cr_return(int ret, Handle new_args, Handle new_props, Handle err_codes, Handle err_msgs, TRAPS) {
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
    return cr_return(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  if (-1 == os::mkdir(CRaCCheckpointTo) && errno != EEXIST) {
    warning("cannot create %s: %s", CRaCCheckpointTo, os::strerror(errno));
    return cr_return(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  Universe::heap()->set_cleanup_unused(true);
  Universe::heap()->collect(GCCause::_full_gc_alot);
  Universe::heap()->set_cleanup_unused(false);
  Universe::heap()->finish_collection();

  if (os::can_trim_native_heap()) {
    os::size_change_t sc;
    if (os::trim_native_heap(&sc)) {
      if (sc.after != SIZE_MAX) {
        const size_t delta = sc.after < sc.before ? (sc.before - sc.after) : (sc.after - sc.before);
        const char sign = sc.after < sc.before ? '-' : '+';
        log_info(crac)("Trim native heap before checkpoint: " PROPERFMT "->" PROPERFMT " (%c" PROPERFMT ")",
                        PROPERFMTARGS(sc.before), PROPERFMTARGS(sc.after), sign, PROPERFMTARGS(delta));
      }
    }
  }

  AsyncLogWriter* aio_writer = AsyncLogWriter::instance();
  if (aio_writer) {
    aio_writer->stop();
  }
  LogConfiguration::close();

  VM_Crac cr(fd_arr, obj_arr, dry_run, (bufferedStream*)jcmd_stream);

  // TODO make these consts configurable
  constexpr int RETRIES_NUM = 10;
  constexpr int RETRY_TIMEOUT_MS = 100;
  for (int i = 0; i <= RETRIES_NUM; i++) {
    {
      MutexLocker ml(Heap_lock);
      VMThread::execute(&cr);
    }
    if (cr.outcome() != VM_Crac::Outcome::RETRY) {
      break;
    }
    if (i < RETRIES_NUM) {
      warning("Retry %i/%i in %i ms...", i + 1, RETRIES_NUM, RETRY_TIMEOUT_MS);
      os::naked_short_sleep(RETRY_TIMEOUT_MS);
    }
  }

  LogConfiguration::reopen();
  if (aio_writer) {
    aio_writer->resume();
  }

  if (cr.outcome() == VM_Crac::Outcome::OK) {
    oop new_args = NULL;
    if (cr.new_args()) {
      new_args = java_lang_String::create_oop_from_str(cr.new_args(), CHECK_NH);
    }
    const GrowableArray<const char *>* new_properties = cr.new_properties();
    objArrayOop propsObj = oopFactory::new_objArray(vmClasses::String_klass(), new_properties->length(), CHECK_NH);
    objArrayHandle props(THREAD, propsObj);

    for (int i = 0; i < new_properties->length(); i++) {
      oop propObj = java_lang_String::create_oop_from_str(new_properties->at(i), CHECK_NH);
      props->obj_at_put(i, propObj);
    }

    wakeup_threads_in_timedwait();

    return cr_return(JVM_CHECKPOINT_OK, Handle(THREAD, new_args), props, Handle(), Handle(), THREAD);
  }

  const GrowableArray<CracFailDep>* failures = cr.failures();

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

  return cr_return(JVM_CHECKPOINT_ERROR, Handle(), Handle(), codes, msgs, THREAD);
}

void crac::restore() {
  assert(!is_portable_mode(), "Use crac::restore_portable() instead");

  jlong restore_time = os::javaTimeMillis();
  jlong restore_nanos = os::javaTimeNanos();

  compute_crengine();

  const int id = os::current_process_id();

  CracSHM shm(id);
  int shmfd = shm.open(O_RDWR | O_CREAT);
  if (0 <= shmfd) {
    if (CracRestoreParameters::write_to(
          shmfd,
          Arguments::jvm_flags_array(), Arguments::num_jvm_flags(),
          Arguments::system_properties(),
          Arguments::java_command() ? Arguments::java_command() : "",
          restore_time,
          restore_nanos)) {
      char strid[32];
      snprintf(strid, sizeof(strid), "%d", id);
      LINUX_ONLY(setenv("CRAC_NEW_ARGS_ID", strid, true));
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
  ::_restore_start_nanos = hdr->_restore_nanos;

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
    guarantee(result == JVMFlag::Error::SUCCESS, "VM Option '%s' cannot be changed: %d",
        name, result);
  }

  for (int i = 0; i < hdr->_nprops; i++) {
    assert((cursor + strlen(cursor) <= contents + st.st_size), "property length exceeds shared memory size");
    int idx = _properties->append(cursor);
    size_t prop_len = strlen(cursor) + 1;
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

// Restore classes and objects in portable mode.
void crac::restore_data(TRAPS) {
  assert(is_portable_mode(), "Use crac::restore() instead");
  precond(CRaCRestoreFrom != nullptr);

  // This is a temporary hack to fix some inconsistencies between the state of
  // pre-initialized classes and restored ones
  // TODO remove this when we get rid of pre-initialized classes
  {
    const TempNewSymbol name = SymbolTable::new_symbol("java/lang/invoke/BoundMethodHandle");
    Klass *const klass = SystemDictionary::resolve_or_fail(name, true, CHECK);
    InstanceKlass::cast(klass)->initialize(CHECK);
  }

  // Create a top-level resource mark to be able to get resource-allocated
  // strings (e.g. external class names) for assert/guarantee fails with no fuss
  assert(Thread::current()->current_resource_mark() == nullptr, "no need for this mark?");
  ResourceMark rm;

  char path[JVM_MAXPATHLEN];

  ParsedHeapDump heap_dump;
  os::snprintf_checked(path, sizeof(path), "%s%s%s", CRaCRestoreFrom, os::file_separator(), PMODE_HEAP_DUMP_FILENAME);
  const char *err_str = HeapDumpParser::parse(path, &heap_dump);
  if (err_str != nullptr) {
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
              err_msg("Cannot parse heap dump %s (%s)", path, err_str));
  }
  assert(!heap_dump.utf8s.contains(HeapDump::NULL_ID) &&
         !heap_dump.class_dumps.contains(HeapDump::NULL_ID) &&
         !heap_dump.instance_dumps.contains(HeapDump::NULL_ID) &&
         !heap_dump.obj_array_dumps.contains(HeapDump::NULL_ID) &&
         !heap_dump.prim_array_dumps.contains(HeapDump::NULL_ID),
         "records cannot have null ID");

  auto *const stack_dump = new ParsedCracStackDump();
  os::snprintf_checked(path, sizeof(path), "%s%s%s", CRaCRestoreFrom, os::file_separator(), PMODE_STACK_DUMP_FILENAME);
  err_str = CracStackDumpParser::parse(path, stack_dump);
  if (err_str != nullptr) {
    delete stack_dump;
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
              err_msg("Cannot parse stack dump %s (%s)", path, err_str));
  }
  if (stack_dump->word_size() != oopSize) {
    const u2 dumped_word_size = stack_dump->word_size();
    delete stack_dump;
    THROW_MSG(vmSymbols::java_lang_UnsupportedOperationException(),
              err_msg("Cannot restore because stack dump comes from an incompatible platform: "
                      "dumped word size %i != current word size %i", dumped_word_size, oopSize));
  }
  STATIC_ASSERT(oopSize == sizeof(intptr_t)); // Need this to safely cast primititve stack values

  // Use heap allocation to allow the class parser to use resource area for internal purposes
  HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> instance_classes(107, 10000);
  HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> array_classes(107, 10000);

  CracHeapRestorer heap_restorer(heap_dump, instance_classes, array_classes, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    delete stack_dump;
    return;
  }

  os::snprintf_checked(path, sizeof(path), "%s%s%s", CRaCRestoreFrom, os::file_separator(), PMODE_CLASS_DUMP_FILENAME);
  HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> class_infos(107, 10000);
  CracClassDumpParser::parse(path, heap_dump, &heap_restorer, &instance_classes, &array_classes, &class_infos, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    delete stack_dump;
    return;
  }

  heap_restorer.restore_heap(class_infos, stack_dump->stack_traces(), THREAD); // Also resolves stack values
  if (HAS_PENDING_EXCEPTION) {
    delete stack_dump;
    return;
  }

  // Resolve all methods on the stacks while we have the ID-to-class and ID-to-symbol mappings
  for (const auto &stack : stack_dump->stack_traces()) {
    for (u4 i = 0; i < stack->frames_num(); i++) {
      stack->frame(i).resolve_method(instance_classes, heap_dump.utf8s, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        delete stack_dump;
        return;
      }
    }
  }

  // Save the stacks for thread restoration
  precond(_stack_dump == nullptr);
  _stack_dump = stack_dump;
}

void crac::restore_threads(TRAPS) {
  assert(is_portable_mode(), "use crac::restore() instead");
  precond(CRaCRestoreFrom != nullptr);
  assert(_stack_dump != nullptr, "call crac::restore_heap() first");
  assert(java_lang_Thread::thread_id(JavaThread::current()->threadObj()) == 1, "must be called on the main thread");

  // The main thread is the first one in the dump (if it's there at all)
  CracStackTrace *main_stack_trace = nullptr;
  if (_stack_dump->stack_traces().is_nonempty()) {
    CracStackTrace *const first_stack_trace = _stack_dump->stack_traces().first();
    const oop first_thread_obj = JNIHandles::resolve_non_null(first_stack_trace->thread());
    if (first_thread_obj == JavaThread::current()->threadObj()) {
      main_stack_trace = first_stack_trace;
      _stack_dump->stack_traces().delete_at(0); // Efficient but changes the order of stack traces
    }
  }

  while (_stack_dump->stack_traces().is_nonempty()) {
    CracStackTrace *const stack_trace = _stack_dump->stack_traces().pop();
    CracThreadRestorer::prepare_thread(stack_trace, CHECK);
  }

  delete _stack_dump; // Is empty by now
  _stack_dump = nullptr;

  CracThreadRestorer::start_prepared_threads();

  if (main_stack_trace != nullptr) {
    CracThreadRestorer::restore_current_thread(main_stack_trace, CHECK);
  }
}
