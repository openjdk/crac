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
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "interpreter/bytecodes.hpp"
#include "interpreter/interpreter.hpp"
#include "jni.h"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/allocation.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/oopsHierarchy.hpp"
#include "os.inline.hpp"
#include "runtime/crac.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/cracHeapRestorer.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/cracStackDumper.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/handles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/reflectionUtils.hpp"
#include "runtime/signature.hpp"
#include "runtime/stackValue.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vframeArray.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/bitCast.hpp"
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

    return ret_cr(JVM_CHECKPOINT_OK, Handle(THREAD, new_args), props, Handle(), Handle(), THREAD);
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

  return ret_cr(JVM_CHECKPOINT_ERROR, Handle(), Handle(), codes, msgs, THREAD);
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

// Restore in portable mode.
void crac::restore_heap(TRAPS) {
  assert(is_portable_mode(), "Use crac::restore() instead");
  precond(CRaCRestoreFrom != nullptr);

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

  precond(_stack_dump == nullptr);
  _stack_dump = stack_dump;
}

class vframeRestoreArrayElement : public vframeArrayElement {
 public:
  void fill_in(const CracStackTrace::Frame &snapshot, bool reexecute) {
    _method = snapshot.method();

    _bci = snapshot.bci();
    guarantee(_method->validate_bci(_bci) == _bci, "invalid bytecode index %i", _bci);

    _reexecute = reexecute;

    _locals = stack_values_from_frame(snapshot.locals());
    _expressions = stack_values_from_frame(snapshot.operands());

    // TODO add monitor info into the snapshot; for now assuming no monitors
    _monitors = nullptr;
    DEBUG_ONLY(_removed_monitors = false;)
  }

 private:
  static StackValueCollection *stack_values_from_frame(const GrowableArrayCHeap<CracStackTrace::Frame::Value, mtInternal> &src) {
    auto *const stack_values = new StackValueCollection(src.length()); // size == 0 until we actually add the values
    // Cannot use the array iterator as it creates copies and we cannot copy
    // resolved reference values in this scope (it requires a Handle allocation)
    for (int i = 0; i < src.length(); i++) {
      const auto &src_value = *src.adr_at(i);
      switch (src_value.type()) {
        // At checkpoint this was either a T_INT or a T_CONFLICT StackValue,
        // in the later case it should have been dumped as 0 for us
        case CracStackTrace::Frame::Value::Type::PRIM: {
          // We've checked that stack slot size of the dump equals ours (right
          // after parsing), so the cast is safe
          LP64_ONLY(const u8 val = src_value.as_primitive());  // Take the whole u8
          NOT_LP64(const u4 val = src_value.as_primitive());   // Take the low half
          const auto int_stack_slot = bit_cast<intptr_t>(val); // 4 or 8 byte slot depending on the platform
          stack_values->add(new StackValue(int_stack_slot));
          break;
        }
        // At checkpoint this was a T_OBJECT StackValue
        case CracStackTrace::Frame::Value::Type::OBJ: {
          const oop o = JNIHandles::resolve(src_value.as_obj());
          // Unpacking code of vframeArrayElement expects a raw oop
          stack_values->add(new StackValue(cast_from_oop<intptr_t>(o), T_OBJECT));
          break;
        }
        default:
          ShouldNotReachHere();
      }
    }
    return stack_values;
  }
};

class vframeRestoreArray : public vframeArray {
 public:
  static vframeRestoreArray *allocate(const CracStackTrace &stack) {
    guarantee(stack.frames_num() <= INT_MAX, "stack trace of thread " SDID_FORMAT " is too long: " UINT32_FORMAT " > %i",
              stack.thread_id(), stack.frames_num(), INT_MAX);
    auto *const result = reinterpret_cast<vframeRestoreArray *>(AllocateHeap(sizeof(vframeRestoreArray) + // fixed part
                                                                             sizeof(vframeRestoreArrayElement) * (stack.frames_num() - 1), // variable part
                                                                             mtInternal));
    result->_frames = static_cast<int>(stack.frames_num());
    result->set_unroll_block(nullptr); // The actual value should be set by the caller later

    // We don't use these
    result->_owner_thread = nullptr; // Would have been JavaThread::current()
    result->_sender = frame();       // Will be the CallStub frame called before the restored frames
    result->_caller = frame();       // Seems to be the same as _sender
    result->_original = frame();     // Deoptimized frame which we don't have

    result->fill_in(stack);
    return result;
  }

 private:
  void fill_in(const CracStackTrace &stack) {
    _frame_size = 0; // Unused (no frame is being deoptimized)

    // vframeRestoreArray: the first frame is the youngest, the last is the oldest
    // CracStackTrace:     the first frame is the oldest, the last is the youngest
    log_trace(crac)("Filling stack trace for thread " SDID_FORMAT, stack.thread_id());
    precond(frames() == checked_cast<int>(stack.frames_num()));
    for (int i = 0; i < frames(); i++) {
      log_trace(crac)("Filling frame %i", i);
      auto *const elem = static_cast<vframeRestoreArrayElement *>(element(i));
      // Note: youngest frame's BCI is always re-executed -- this is important
      // because otherwise deopt's unpacking code will try to use ToS caching
      // which we don't account for
      elem->fill_in(stack.frame(frames() - 1 - i), /*reexecute when youngest*/ i == 0);
      assert(!elem->method()->is_native(), "native methods are not restored");
    }
  }
};

// Called by RestoreBlob to get the info about the frames to restore. This is
// analogous to Deoptimization::fetch_unroll_info() except that we fetch the
// info from the stack snapshot instead of a deoptee frame. This is also a leaf
// (in contrast with fetch_unroll_info) since no reallocation is needed (see the
// comment before fetch_unroll_info).
JRT_LEAF(Deoptimization::UnrollBlock *, crac::fetch_frame_info(JavaThread *current))
  precond(current == JavaThread::current());
  log_debug(crac)("Thread " UINTX_FORMAT ": fetching frame info", cast_from_oop<uintptr_t>(current->threadObj()));

  // Heap-allocated resource mark to use resource-allocated StackValues
  // and free them before starting executing the restored code
  guarantee(current->deopt_mark() == nullptr, "No deopt should be pending");
  current->set_deopt_mark(new DeoptResourceMark(current));

  // Create vframe descriptions based on the stack snapshot -- no safepoint
  // should happen after this array is filled until we're done with it
  vframeRestoreArray *array;
  {
    const CracStackTrace *stack = _stack_dump->stack_traces().pop();
    assert(stack->frames_num() > 0, "should be checked when just starting");
    if (_stack_dump->stack_traces().is_empty()) {
      delete _stack_dump;
      _stack_dump = nullptr;
    }

    array = vframeRestoreArray::allocate(*stack);
    postcond(array->frames() == static_cast<int>(stack->frames_num()));

    delete stack;
  }
  postcond(array->frames() > 0);
  log_debug(crac)("Thread " UINTX_FORMAT ": filled frame array (%i frames)",
                  cast_from_oop<uintptr_t>(current->threadObj()), array->frames());

  // Determine sizes and return pcs of the constructed frames.
  //
  // The order of frames is the reverse of the array above:
  // frame_sizes and frame_pcs: 0th -- the oldest frame,   nth -- the youngest.
  // vframeRestoreArray *array: 0th -- the youngest frame, nth -- the oldest.
  auto *const frame_sizes = NEW_C_HEAP_ARRAY(intptr_t, array->frames(), mtInternal);
  // +1 because the last element is an address to jump into the interpreter
  auto *const frame_pcs = NEW_C_HEAP_ARRAY(address, array->frames() + 1, mtInternal);
  // Create an interpreter return address for the assembly code to use as its
  // return address so the skeletal frames are perfectly walkable
  frame_pcs[array->frames()] = Interpreter::deopt_entry(vtos, 0);

  // We start from the youngest frame, which has no callee
  int callee_params = 0;
  int callee_locals = 0;
  for (int i = 0; i < array->frames(); i++) {
    // Deopt code uses this to account for possible JVMTI's PopFrame function
    // usage which is irrelevant in our case
    static constexpr int popframe_extra_args = 0;

    // i == 0 is the youngest frame, i == array->frames() - 1 is the oldest
    frame_sizes[array->frames() - i - 1] =
        BytesPerWord * array->element(i)->on_stack_size(callee_params, callee_locals, i == 0, popframe_extra_args);

    frame_pcs[array->frames() - i - 1] = i < array->frames() - 1 ?
    // Setting the pcs the same way as the deopt code does. It is needed to
    // identify the skeleton frames as interpreted and make them walkable. The
    // correct pcs will be patched later when filling the frames.
                                         Interpreter::deopt_entry(vtos, 0) - frame::pc_return_offset :
    // The oldest frame always returns to CallStub
                                         StubRoutines::call_stub_return_address();

    callee_params = array->element(i)->method()->size_of_parameters();
    callee_locals = array->element(i)->method()->max_locals();
  }

  // Adjustment of the CallStub to accomodate the locals of the oldest restored
  // frame, if any
  const int caller_adjustment = Deoptimization::last_frame_adjust(callee_params, callee_locals);

  auto *const info = new Deoptimization::UnrollBlock(
    0,                           // Deoptimized frame size, unused (no frame is being deoptimized)
    caller_adjustment * BytesPerWord,
    0,                           // Amount of params in the CallStub frame, unused (known via the oldest frame's method)
    array->frames(),
    frame_sizes,
    frame_pcs,
    BasicType::T_ILLEGAL,        // Return type, unused (we are not in the process of returning a value)
    Deoptimization::Unpack_deopt // fill_in_frames() always specifies Unpack_deopt, regardless of what's set here
  );
  array->set_unroll_block(info);

  guarantee(current->vframe_array_head() == nullptr, "no deopt should be pending");
  current->set_vframe_array_head(array);

  return info;
JRT_END

// Called by RestoreBlob after skeleton frames have been pushed on stack to fill
// them. This is analogous to Deoptimization::unpack_frames().
JRT_LEAF(void, crac::fill_in_frames(JavaThread *current))
  precond(current == JavaThread::current());
  log_debug(crac)("Thread " UINTX_FORMAT ": filling skeletal frames", cast_from_oop<uintptr_t>(current->threadObj()));

  // Reset NoHandleMark created by JRT_LEAF (see related comments in
  // Deoptimization::unpack_frames() on why this is ok). Handles are used e.g.
  // in trace printing.
  ResetNoHandleMark rnhm;
  HandleMark hm(current);

  // Array created by crac::restore_thread_state()
  vframeArray* array = current->vframe_array_head();
  // Java frame between the skeleton frames and the frame of this function
  frame unpack_frame = current->last_frame();
  // Amount of parameters in the CallStub frame = amount of parameters of the
  // oldest skeleton frame
  int initial_caller_parameters = array->element(array->frames() - 1)->method()->size_of_parameters();

  // TODO save, clear, restore last Java sp like the deopt code does?

  assert(current->deopt_compiled_method() == nullptr, "no method is being deoptimized");
  guarantee(current->frames_to_pop_failed_realloc() == 0,
            "we don't deoptimize, so no reallocations of scalar replaced objects can happen and fail");
  array->unpack_to_stack(unpack_frame, Deoptimization::Unpack_deopt /* TODO this or reexecute? */, initial_caller_parameters);
  log_debug(crac)("Thread " UINTX_FORMAT": skeletal frames filled", cast_from_oop<uintptr_t>(current->threadObj()));

  // Cleanup, analogous to Deoptimization::cleanup_deopt_info()
  current->set_vframe_array_head(nullptr);
  delete array->unroll_block(); // Also deletes frame_sizes and frame_pcs
  delete array;
  delete current->deopt_mark();
  current->set_deopt_mark(nullptr);

  // TODO more verifications, like the ones Deoptimization::unpack_frames() does
  DEBUG_ONLY(current->validate_frame_layout();)
JRT_END

// Make this second-youngest frame the youngest faking the result of the
// callee (i.e. the current youngest) frame.
static void transform_to_youngest(CracStackTrace::Frame *frame, Handle callee_result) {
  const Bytecodes::Code code = frame->method()->code_at(frame->bci());
  assert(Bytecodes::is_invoke(code), "non-youngest frames stay must be invoking, got %s", Bytecodes::name(code));

  // Push the result onto the operand stack
  if (callee_result.not_null()) {
    const auto operands_num = frame->operands().length();
    assert(operands_num < frame->method()->max_stack(), "cannot push return value: all %i slots taken",
           frame->method()->max_stack());
    frame->operands().reserve(operands_num + 1); // Not bare append because it may allocate more than one slot
    // FIXME append() creates a copy but accepts a reference so no copy elision can occur
    frame->operands().append({}); // Cheap empty->empty copy, empty->empty swap
    *frame->operands().adr_at(operands_num) = CracStackTrace::Frame::Value::of_obj(callee_result); // Cheap resolved->empty swap
  }

  // Increment the BCI past the invoke bytecode
  const int code_len = Bytecodes::length_for(code);
  assert(code_len > 0, "invoke codes don't need special length calculation");
  frame->set_bci(frame->bci() + code_len);
  assert(frame->method()->validate_bci(frame->bci()) >= 0, "transformed to invalid BCI %i", frame->bci());
}

// If the youngest frame represents special method requiring a fixup, applies
// the fixup. If all frames get popped, the return value is returned.
static JavaValue fixup_youngest_frame_if_special(CracStackTrace *stack, TRAPS) {
  precond(stack->frames_num() > 0);

  const Method &youngest_m = *stack->frame(stack->frames_num() - 1).method();
  if (!youngest_m.is_native()) { // Only native methods are special
    return {};
  }
  const InstanceKlass &holder = *youngest_m.method_holder();

  if (holder.name() == vmSymbols::jdk_crac_Core() && holder.class_loader_data()->is_the_null_class_loader_data() &&
      youngest_m.name() == vmSymbols::checkpointRestore0_name()) { // Checkpoint initiation method
    // Pop the native frame
    stack->pop();

    // Create the return value indicating the successful restoration
    HandleMark hm(Thread::current()); // The handle will either become an oop or a JNI handle
    const Handle bundle_h = ret_cr(JVM_CHECKPOINT_OK, Handle(), Handle(), Handle(), Handle(), CHECK_({}));

    if (stack->frames_num() == 0) {
      // No Java caller (e.g. called from JNI), return the value directly
      precond(bundle_h->is_array());
      JavaValue bundle_jv(T_ARRAY);
      bundle_jv.set_oop(bundle_h());
      return bundle_jv;
    }

    // Push the return value onto the caller's operand stack
    CracStackTrace::Frame &caller = stack->frame(stack->frames_num() - 1);
    transform_to_youngest(&caller, bundle_h);
  } else {
    assert(!youngest_m.is_native(), "only special native methods can be restored");
  }

  return {};
}

// Fills the provided arguments with null-values according to the provided
// signature.
class NullArgumentsFiller : public SignatureIterator {
 public:
  NullArgumentsFiller(Symbol *signature, JavaCallArguments *args) : SignatureIterator(signature), _args(args) {
    precond(args->size_of_parameters() == 0);
    do_parameters_on(this);
  }

 private:
  JavaCallArguments *_args;

  friend class SignatureIterator;  // so do_parameters_on can call do_type

  void do_type(BasicType type) {
    switch (type) {
      case T_BYTE:
      case T_BOOLEAN:
      case T_CHAR:
      case T_SHORT:
      case T_INT:    _args->push_int(0);        break;
      case T_FLOAT:  _args->push_float(0);      break;
      case T_LONG:   _args->push_long(0);       break;
      case T_DOUBLE: _args->push_double(0);     break;
      case T_ARRAY:
      case T_OBJECT: _args->push_oop(Handle()); break;
      default:       ShouldNotReachHere();
    }
  }
};


// Initiates thread restoration and won't return until the restored execution
// completes. Returns the result of the execution. If the stack was empty, the
// result will have type T_ILLEGAL.
//
// The process of thread restoration is as follows:
// 1. This method is called to make a Java-call to the initial method (the
// oldest one in the stack) with the snapshotted arguments, replacing its entry
// point with an entry into assembly restoration code (RestoreBlob).
// 2. Java-call places a CallStub frame for the initial method and calls
// RestoreBlob.
// 3. RestoreBlob calls crac::fetch_frame_info() which prepares restoration info
// based on the stack snapshot. This cannot be perfomed directly in step 1:
// a safepoint can occur on step 2 which the prepared data won't survive.
// 4. RestoreBlob reads the prepared restoration info and creates so-called
// skeletal frames which are walkable interpreter frames of proper sizes but
// with monitors, locals, expression stacks, etc. unfilled.
// 5. RestoreBlob calls crac::fill_in_frames() which also reads the prepared
// restoration info and fills the skeletal frames.
// 6. RestoreBlob jumps into the interpreter to start executing the youngest
// restored stack frame.
JavaValue crac::restore_current_thread(TRAPS) {
  precond(!_stack_dump->stack_traces().is_empty());
  JavaThread *const current = JavaThread::current();
  if (log_is_enabled(Info, crac)) {
    ResourceMark rm;
    log_info(crac)("Thread " UINTX_FORMAT " (%s): starting the restoration",
                   cast_from_oop<uintptr_t>(current->threadObj()), current->name());
  }

  // If the stack is empty there is nothing to restore
  // TODO should this be considered an error?
  CracStackTrace *const stack = _stack_dump->stack_traces().last();
  if (stack->frames_num() == 0) {
    log_info(crac)("Thread " UINTX_FORMAT ": no frames in stack snapshot (ID " SDID_FORMAT ")",
                    cast_from_oop<uintptr_t>(current->threadObj()), stack->thread_id());
    _stack_dump->stack_traces().pop();
    if (_stack_dump->stack_traces().is_empty()) {
      delete _stack_dump;
      _stack_dump = nullptr;
    }
    delete stack;
    return {};
  }

  { // Check if there are special frames requiring fixup, this may pop some frames
    const JavaValue result = fixup_youngest_frame_if_special(stack, CHECK_({}));
    if (stack->frames_num() == 0) {
      assert(result.get_type() != T_ILLEGAL, "return value must be initialized");
      log_info(crac)("Thread " UINTX_FORMAT ": all frames have been popped as special",
                     cast_from_oop<uintptr_t>(current->threadObj()));
      delete stack;
      return result;
    }
  }

  const CracStackTrace::Frame &oldest_frame = stack->frame(0);
  Method *const method = oldest_frame.method();

  JavaCallArguments args;
  // The actual values will be filled by the RestoreStub, we just need the Java
  // call code to allocate the right amount of space
  // TODO tell Java call the required size directly without generating the
  // actual arguments like this
  NullArgumentsFiller(method->signature(), &args);
  // Make the CallStub call RestoreStub instead of the actual method entry
  args.set_use_restore_stub(true);

  if (log_is_enabled(Info, crac)) {
    ResourceMark rm;
    log_debug(crac)("Thread " UINTX_FORMAT ": restoration starts from %s",
                    cast_from_oop<uintptr_t>(current->threadObj()), method->external_name());
  }
  JavaValue result(method->result_type());
  JavaCalls::call(&result, methodHandle(current, method), &args, CHECK_({}));
  // The stack snapshot has been freed already by now

  log_info(crac)("Thread " UINTX_FORMAT ": restored execution completed",
                 cast_from_oop<uintptr_t>(current->threadObj()));
  return result;
}

void crac::restore_threads(TRAPS) {
  assert(is_portable_mode(), "use crac::restore() instead");
  precond(CRaCRestoreFrom != nullptr);
  assert(_stack_dump != nullptr, "call crac::restore_heap() first");

  // TODO for now we only restore the main thread
  assert(_stack_dump->stack_traces().length() == 1, "expected only a single (main) thread to be dumped");
#ifdef ASSERT
  {
    ResourceMark rm; // Thread name
    assert(java_lang_Thread::threadGroup(JavaThread::current()->threadObj()) == Universe::main_thread_group() &&
           strcmp(JavaThread::current()->name(), "main") == 0, "must be called on the main thread");
  }
#endif // ASSERT
  JavaValue result = restore_current_thread(CHECK);
  log_info(crac)("main thread execution resulted in type: %s", type2name(result.get_type()));
}
