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
#include "classfile/vmSymbols.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/oopFactory.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "os.inline.hpp"
#include "runtime/crac.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/handles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/reflectionUtils.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/decoder.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/heapDumpParser.hpp"
#ifdef LINUX
#include "os_linux.hpp"
#endif

static const char PMODE_HEAP_DUMP_FILENAME[] = "heap.hprof";

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

bool crac::is_portable_mode() {
  return CREngine == nullptr;
}

// Checkpoint in portable mode.
static void checkpoint_portable() {
  char path[JVM_MAXPATHLEN];
  os::snprintf_checked(path, sizeof(path), "%s%s%s", CRaCCheckpointTo,
                       os::file_separator(), PMODE_HEAP_DUMP_FILENAME);

  HeapDumper dumper(false /* No GC: it's already done by crac::checkpoint */);
  if (dumper.dump(path,
                  nullptr,  // No additional output
                  -1,       // No compression, TODO: enable this when the parser supports it
                  false,    // Don't overwrite
                  HeapDumper::default_num_of_dump_threads()) != 0) {
    ResourceMark rm;
    warning("Failed to dump heap into %s for checkpoint: %s",
            path, dumper.error_as_C_string());
  }
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
  assert(CREngine != nullptr, "Portable mode requested, should not call this");

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
    _ok = ok;
    return;
  }

  if (!crac::is_portable_mode() && !memory_checkpoint()) {
    return;
  }

  int shmid = 0;
  if (CRAllowToSkipCheckpoint) {
    trace_cr("Skip Checkpoint");
  } else {
    trace_cr("Checkpoint ...");
    report_ok_to_jcmd_if_any();
    if (crac::is_portable_mode()) {
      checkpoint_portable();
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

static void init_basic_type_mirror_names(TRAPS) {
  for (u1 t = T_BOOLEAN; t <= T_LONG; t++) {
    Handle mirror = Handle(JavaThread::current(), Universe::java_mirror(static_cast<BasicType>(t)));
    java_lang_Class::name(mirror, CHECK);
  }
  java_lang_Class::name(Handle(JavaThread::current(), Universe::void_mirror()), CHECK);
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

  if (is_portable_mode()) {
    // Trigger name field initialization for Class<*> instances of basic types
    // so that these can be differentiated upon the restoration
    // TODO figure out a more robust way to achieve this differentiation
    init_basic_type_mirror_names(CHECK_NH);
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
  {
    MutexLocker ml(Heap_lock);
    VMThread::execute(&cr);
  }

  LogConfiguration::reopen();
  if (aio_writer) {
    aio_writer->resume();
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

    wakeup_threads_in_timedwait();

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

void crac::restore() {
  assert(!is_portable_mode(), "Use crac::restore_portable");

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

// Has the following assumptions about the heap dump:
// 1. Nulls are represented by 0 ID.
// 2. Class<*> instances are included into instance dumps only for primitive
// types, and their name field is initialized.
//
// These assumptions are met when reading dumps generated by HeapDumper.
class HeapRestorer : public StackObj {
 private:
  // HPROF does not have a special notion for a null reference. We will trear 0
  // ID as such.
  static const HeapDumpFormat::id_t NULL_ID = 0;

 public:
  explicit HeapRestorer(const ParsedHeapDump &heap_dump) : _heap_dump(heap_dump) {};

  void restore_heap(TRAPS) {
    // For now we rely on CDS to pre-initialize the built-in class loaders
    if (SystemDictionary::java_platform_loader() == nullptr || SystemDictionary::java_system_loader() == nullptr) {
      Unimplemented();
    }

    // Look through the dump to find platform and system class loaders' IDs
    find_base_class_loader_ids(CHECK);

    _heap_dump.class_dump_records.iterate(
      [&](HeapDumpFormat::id_t _, const HeapDumpFormat::ClassDumpRecord &dump) -> bool {
        // For now we only restore user-provided classes
        if (dump.class_loader_id == _system_class_loader_id) {
          restore_class(dump, CHECK_false);
        }
        return true;
      }
    );
    if (HAS_PENDING_EXCEPTION) return;

    // TODO this can be removed after object restoration is implemented, since
    // these are just subsets of all dumped objects
    {
      auto restore_iter = [&](HeapDumpFormat::id_t id, const instanceHandle &_) -> bool {
        restore_object(id, CHECK_false);
        return true;
      };
      _prepared_class_loaders.iterate(restore_iter);
      if (HAS_PENDING_EXCEPTION) return;
      _allocated_prot_domains.iterate(restore_iter);
      if (HAS_PENDING_EXCEPTION) return;
    }
    // TODO restore objects
  }

 private:
  const ParsedHeapDump &_heap_dump;

  Symbol *get_dumped_symbol(HeapDumpFormat::id_t id, TRAPS) const {
    HeapDumpFormat::UTF8Record *utf8 = _heap_dump.utf8_records.get(id);
    if (utf8 != nullptr) return utf8->sym;
    THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
               err_msg("UTF-8 record " INT64_FORMAT " referenced but absent", id), {});
  }

  Symbol *get_dumped_class_name(HeapDumpFormat::id_t class_id, TRAPS) const {
    HeapDumpFormat::LoadClassRecord *lcr = _heap_dump.load_class_records.get(class_id);
    if (lcr == nullptr) {
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                     err_msg("Class loading record " INT64_FORMAT " referenced but absent", class_id));
    }
    Symbol *name = get_dumped_symbol(lcr->class_name_id, CHECK_NULL);
    return name;
  }

  HeapDumpFormat::ClassDumpRecord *get_class_dump(HeapDumpFormat::id_t id, TRAPS) const {
    HeapDumpFormat::ClassDumpRecord *dump = _heap_dump.class_dump_records.get(id);
    if (dump != nullptr) return dump;
    THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Class dump " INT64_FORMAT " referenced but absent", id));
  }

  HeapDumpFormat::InstanceDumpRecord *get_instance_dump(HeapDumpFormat::id_t id, TRAPS) const {
    HeapDumpFormat::InstanceDumpRecord *dump = _heap_dump.instance_dump_records.get(id);
    if (dump != nullptr) return dump;
    THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Instance dump " INT64_FORMAT " referenced but absent", id));
  }

  HeapDumpFormat::id_t _platform_class_loader_id = NULL_ID;
  HeapDumpFormat::id_t _builtin_app_class_loader_id = NULL_ID;
  HeapDumpFormat::id_t _system_class_loader_id = NULL_ID;

  // Finds platform and system class loaders' IDs in the dump.
  void find_base_class_loader_ids(TRAPS) {
    _heap_dump.load_class_records.iterate([&](HeapDumpFormat::id_t _, const HeapDumpFormat::LoadClassRecord &lcr) -> bool {
      Symbol *name = get_dumped_symbol(lcr.class_name_id, CHECK_false);
      if (name == vmSymbols::java_lang_ClassLoader()) {
        set_system_class_loader_id(lcr, CHECK_false);
      } else if (name ==  vmSymbols::jdk_internal_loader_ClassLoaders()) {
        set_builtin_class_loader_ids(lcr, CHECK_false);
      }
      return true;
    });

    if (HAS_PENDING_EXCEPTION) {
      Handle e(Thread::current(), PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      THROW_MSG_CAUSE(vmSymbols::java_lang_IllegalArgumentException(), "Cannot find dumped built-in class loaders", e);
    }
    if (_platform_class_loader_id == NULL_ID ||
        !_heap_dump.instance_dump_records.contains(_platform_class_loader_id)) {
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Cannot find dumped platform class loader");
    }
    if (_builtin_app_class_loader_id == NULL_ID ||
        !_heap_dump.instance_dump_records.contains(_builtin_app_class_loader_id)) {
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Cannot find dumped built-in app class loader");
    }
    if (_system_class_loader_id == NULL_ID ||
        !_heap_dump.instance_dump_records.contains(_system_class_loader_id)) {
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Cannot find dumped system class loader");
    }
    if (_platform_class_loader_id == _builtin_app_class_loader_id ||
        _platform_class_loader_id == _system_class_loader_id) {
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                "Platform and system class loaders cannot be dumped as the same instance");
    }
  }

  void set_system_class_loader_id(const HeapDumpFormat::LoadClassRecord &lcr, TRAPS) {
    // This relies on ClassLoader.getSystemClassLoader() implementation detail:
    // the system class loader is stored in scl static field of j.l.ClassLoader
    static const char SCL_FIELD_NAME[] = "scl";

    HeapDumpFormat::ClassDumpRecord *dump = get_class_dump(lcr.class_id, CHECK);
    if (dump->class_loader_id != NULL_ID) {
      // Classes from java.* packages cannot be non-boot-loaded
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Class %s can only be loaded by the bootstrap class loader",
                         vmClasses::ClassLoader_klass()->name()->as_C_string()));
    }

    if (_system_class_loader_id != NULL_ID) {
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Class %s has multiple dumps", vmClasses::ClassLoader_klass()->external_name()));
    }

    for (size_t i = 0; i < dump->static_fields.size(); i++) {
      const HeapDumpFormat::ClassDumpRecord::Field &field_dump = dump->static_fields[i];
      if (field_dump.info.type != HPROF_NORMAL_OBJECT) {
        continue;
      }
      Symbol *field_name = get_dumped_symbol(field_dump.info.name_id, CHECK);
      if (field_name->equals(SCL_FIELD_NAME)) {
        if (_system_class_loader_id != NULL_ID) {
          ResourceMark rm;
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Static field %s is repeated in %s dump " INT64_FORMAT,
                            SCL_FIELD_NAME, vmClasses::ClassLoader_klass()->external_name(), dump->id));
        }
        if (field_dump.value.as_object_id == NULL_ID) {
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Dumped system class loader is null");
        }
        _system_class_loader_id = field_dump.value.as_object_id;
      }
    }

    if (_system_class_loader_id == NULL_ID) {
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Static field %s is missing from %s dump " INT64_FORMAT,
                        SCL_FIELD_NAME, vmClasses::ClassLoader_klass()->external_name(), dump->id));
    }
  }

  void set_builtin_class_loader_ids(const HeapDumpFormat::LoadClassRecord &lcr, TRAPS) {
    // This relies on the ClassLoader.get*ClassLoader() implementation detail:
    // the built-in platform and app class loaders are stored in
    // PLATFORM_LOADER/APP_LOADER static fields of
    // jdk.internal.loader.ClassLoaders
    static const char PLATFORM_LOADER_FIELD_NAME[] = "PLATFORM_LOADER";
    static const char APP_LOADER_FIELD_NAME[] = "APP_LOADER";

    const HeapDumpFormat::ClassDumpRecord *dump = get_class_dump(lcr.class_id, CHECK);
    if (dump->class_loader_id != NULL_ID) {
      // Classes from jdk.* packages can be non-boot-loaded, but we need the one
      // that is
      return;
    }

    if (_platform_class_loader_id != NULL_ID || _builtin_app_class_loader_id != NULL_ID) {
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Class %s has multiple dumps",
                        vmClasses::jdk_internal_loader_ClassLoaders_klass()->external_name()));
    }

    for (size_t i = 0; i < dump->static_fields.size(); i++) {
      const HeapDumpFormat::ClassDumpRecord::Field &field_dump = dump->static_fields[i];
      if (field_dump.info.type != HPROF_NORMAL_OBJECT) {
        continue;
      }
      Symbol *field_name = get_dumped_symbol(field_dump.info.name_id, CHECK);
      if (field_name->equals(PLATFORM_LOADER_FIELD_NAME)) {
        if (_platform_class_loader_id != NULL_ID) {
          ResourceMark rm;
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Static field %s is repeated in %s dump " INT64_FORMAT, PLATFORM_LOADER_FIELD_NAME,
                            vmClasses::jdk_internal_loader_ClassLoaders_klass()->external_name(), dump->id));
        }
        if (field_dump.value.as_object_id == NULL_ID) {
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Dumped platform class loader is null");
        }
        _platform_class_loader_id = field_dump.value.as_object_id;
      } else if (field_name->equals(APP_LOADER_FIELD_NAME)) {
        if (_builtin_app_class_loader_id != NULL_ID) {
          ResourceMark rm;
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Static field %s is repeated in %s dump " INT64_FORMAT, APP_LOADER_FIELD_NAME,
                            vmClasses::jdk_internal_loader_ClassLoaders_klass()->external_name(), dump->id));
        }
        if (field_dump.value.as_object_id == NULL_ID) {
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), "Dumped built-in app class loader is null");
        }
        _builtin_app_class_loader_id = field_dump.value.as_object_id;
      }
    }

    if (_platform_class_loader_id == NULL_ID || _builtin_app_class_loader_id == NULL_ID) {
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Static field %s and/or %s are missing from %s dump " INT64_FORMAT,
                        PLATFORM_LOADER_FIELD_NAME, APP_LOADER_FIELD_NAME, vmClasses::ClassLoader_klass()->external_name(),
                        dump->id));
    }
  }

  ResizeableResourceHashtable<HeapDumpFormat::id_t, instanceHandle, AnyObj::C_HEAP> _prepared_class_loaders {11, 1228891};
  ResizeableResourceHashtable<HeapDumpFormat::id_t, instanceHandle, AnyObj::C_HEAP> _allocated_prot_domains {11, 1228891};
  ResizeableResourceHashtable<HeapDumpFormat::id_t, Klass *, AnyObj::C_HEAP>        _loaded_classes         {107, 1228891};
  ResizeableResourceHashtable<HeapDumpFormat::id_t, Klass *, AnyObj::C_HEAP>        _restored_classes       {107, 1228891};
  ResizeableResourceHashtable<HeapDumpFormat::id_t, Handle, AnyObj::C_HEAP>         _restored_objects       {1009, 1228891};

  // Gets a value or puts an empty-initialized value if the key is absent.
  template <class V>
  V *get_or_put_stub(HeapDumpFormat::id_t id, ResizeableResourceHashtable<HeapDumpFormat::id_t, V, AnyObj::C_HEAP> *table) {
    V *value = table->get(id);
    if (value == nullptr) {
      table->put_when_absent(id, {});
      table->maybe_grow();
    }
    return value;
  }

  // Loads the class without restoring it.
  Klass *load_class(const HeapDumpFormat::ClassDumpRecord &dump, TRAPS) {
    Klass **ready = get_or_put_stub(dump.id, &_loaded_classes);
    if (ready != nullptr) {
      if (*ready == nullptr) {
        THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                       err_msg("Loading curcularity detected for dumped class " INT64_FORMAT, dump.id));
      }
      return *ready;
    }
    log_trace(restore)("Loading class " INT64_FORMAT, dump.id);

    // TODO have to also load interfaces, because otherwise the standard
    // resolution mechanism will be called for them which may call the user code
    if (dump.super_id != NULL_ID) {
      HeapDumpFormat::ClassDumpRecord *super_dump = get_class_dump(dump.super_id, CHECK_NULL);
      load_class(*super_dump, CHECK_NULL);
    }

    Symbol *name = get_dumped_class_name(dump.id, CHECK_NULL);

    instanceHandle class_loader = get_prepared_class_loader(dump.class_loader_id, CHECK_NULL);
    instanceHandle prot_domain = get_allocated_prot_domain(dump.protection_domain_id, CHECK_NULL);

    Klass *klass;
    if (true /* TODO replace with class_loader.is_null() */) {
      klass = SystemDictionary::resolve_or_fail(name, class_loader, prot_domain, false, CHECK_NULL);
    } else {
      klass = define_dumped_class(name, dump, class_loader, prot_domain, CHECK_NULL);
    }

    if (klass->class_loader() != class_loader()) {
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                     err_msg("Class loader specified in class dump " INT64_FORMAT " does not define the class", dump.id));
    }

    if (klass->is_instance_klass()) {
      verify_fields(InstanceKlass::cast(klass), dump, CHECK_NULL);
    }

    _loaded_classes.put(dump.id, klass);
    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Loaded class " INT64_FORMAT " as %s", dump.id, klass->external_name());
    }
    return klass;
  }

  // Returns the class loader with its state partially restored so it can be
  // used for class definition.
  instanceHandle get_prepared_class_loader(HeapDumpFormat::id_t id, TRAPS) {
    precond(vmClasses::ClassLoader_klass()->is_initialized());

    if (id == NULL_ID) {  // Bootstrap class loader
      return instanceHandle();
    }

    instanceHandle *ready = get_or_put_stub(id, &_prepared_class_loaders);
    if (ready != nullptr) {
      if (ready->is_null()) {
        THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Preparation circularity detected for class loader dump " INT64_FORMAT, id), {});
      }
      return *ready;
    }
    log_trace(restore)("Preparing class loader " INT64_FORMAT, id);

    HeapDumpFormat::InstanceDumpRecord *instance_dump = get_instance_dump(id, CHECK_({}));

    InstanceKlass *klass = load_instance_class(instance_dump->class_id, true, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      Handle e(Thread::current(), PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      THROW_MSG_CAUSE_(vmSymbols::java_lang_IllegalArgumentException(),
                       err_msg("Cannot load class of class loader dump " INT64_FORMAT, id), e, {});
    }
    if (!klass->is_class_loader_instance_klass()) {
      ResourceMark rm;
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Class loader class dump " INT64_FORMAT " is loaded as %s which does not extend %s",
                         instance_dump->class_id, klass->external_name(), vmClasses::ClassLoader_klass()->external_name()),
                 {});
    }

    instanceHandle handle = prepare_class_loader(klass, *instance_dump, CHECK_({}));

    _prepared_class_loaders.put(id, handle);
    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Prepared class loader " INT64_FORMAT " (%s)", id, klass->external_name());
    }
    return handle;
  }

  // Partially restores the class loader so it can be used for class definition.
  // If it is dumped as a built-in class loader, it will be set as such.
  instanceHandle prepare_class_loader(InstanceKlass *klass, const HeapDumpFormat::InstanceDumpRecord &dump, TRAPS) {
    precond(klass->is_subclass_of(vmClasses::ClassLoader_klass()));

    if (dump.id == _platform_class_loader_id && SystemDictionary::java_platform_loader() != nullptr) {
      warning("Using platform class loader as created by CDS");
      instanceOop o = reinterpret_cast<instanceOop>(SystemDictionary::java_platform_loader());
      return instanceHandle(JavaThread::current(), o);
    }
    if (dump.id == _system_class_loader_id && SystemDictionary::java_system_loader() != nullptr) {
      if (dump.id != _builtin_app_class_loader_id) {
        THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Dumped system class loader " INT64_FORMAT " is expected to also be "
                           "the built-in app class loader " INT64_FORMAT, dump.id, _builtin_app_class_loader_id), {});
      }
      warning("Using system class loader as created by CDS");
      instanceOop o = reinterpret_cast<instanceOop>(SystemDictionary::java_system_loader());
      return instanceHandle(JavaThread::current(), o);
    }

    // TODO for now, we only use the built-in platform and system class loaders
    // newly constructed by CDS
    Unimplemented();
    return {};
  }

  // Returns an allocated protection domain so it can be used for class
  // definition.
  instanceHandle get_allocated_prot_domain(HeapDumpFormat::id_t id, TRAPS) {
    if (id == NULL_ID) {
      return instanceHandle();
    }
    instanceHandle *ready = get_or_put_stub(id, &_allocated_prot_domains);
    if (ready != nullptr) {
      if (ready->is_null()) {
        THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Preparation circularity detected for protection domain dump " INT64_FORMAT, id), {});
      }
      return *ready;
    }
    log_trace(restore)("Allocating protection domain " INT64_FORMAT, id);

    HeapDumpFormat::InstanceDumpRecord *instance_dump = get_instance_dump(id, CHECK_({}));

    InstanceKlass *klass = load_instance_class(instance_dump->class_id, true, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      Handle e(Thread::current(), PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      THROW_MSG_CAUSE_(vmSymbols::java_lang_IllegalArgumentException(),
                       err_msg("Cannot load class of protection domain dump " INT64_FORMAT, id), e, {});
    }
    if (!klass->is_subclass_of(vmClasses::ProtectionDomain_klass())) {
      ResourceMark rm;
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Protection domain class dump " INT64_FORMAT " is loaded as %s which does not extend %s",
                         instance_dump->class_id, klass->external_name(), vmClasses::ClassLoader_klass()->external_name()),
                 {});
    }

    instanceHandle handle = klass->allocate_instance_handle(CHECK_({}));

    _allocated_prot_domains.put(id, handle);
    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Allocated protection domain " INT64_FORMAT " (%s)", id, klass->external_name());
    }
    return handle;
  }

  InstanceKlass *load_instance_class(HeapDumpFormat::id_t id, bool check_instantiable, TRAPS) {
    HeapDumpFormat::ClassDumpRecord *dump = get_class_dump(id, CHECK_NULL);
    InstanceKlass *ik = load_instance_class(*dump, check_instantiable, CHECK_NULL);
    return ik;
  }

  InstanceKlass *load_instance_class(const HeapDumpFormat::ClassDumpRecord &dump, bool check_instantiable, TRAPS) {
    Klass *k = load_class(dump, CHECK_NULL);
    if (check_instantiable) {
      k->check_valid_for_instantiation(false, CHECK_NULL);
    } else if (!k->is_instance_klass()) {
      ResourceMark rm;
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                     err_msg("%s is not an instance class", k->external_name()));
    }
    return InstanceKlass::cast(k);
  }

  // Verifies that names and basic types of all fields match.
  void verify_fields(InstanceKlass *klass, const HeapDumpFormat::ClassDumpRecord &dump, TRAPS) const {
    FieldStream fs(klass,
                   true /* local fields only: super's fields are verified when processing supers */,
                   true /* no interfaces (if the class is an interface it's still gonna be processed) */);
    size_t static_i = 0;
    size_t instance_i = 0;

    while (!fs.eos()) {
      bool is_static = fs.access_flags().is_static();
      if (is_static && static_i >= dump.static_fields.size()) {
        ResourceMark rm;
        THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                  err_msg("Class %s has more static fields than its dump " INT64_FORMAT,
                          klass->external_name(), dump.id));
      } else if (!is_static && instance_i >= dump.instance_field_infos.size()) {
        ResourceMark rm;
        THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                  err_msg("Class %s has more non-static fields than its dump " INT64_FORMAT,
                          klass->external_name(), dump.id));
      }

      const HeapDumpFormat::ClassDumpRecord::Field::Info &field_info = is_static ?
                                                                       dump.static_fields[static_i++].info :
                                                                       dump.instance_field_infos[instance_i++];
      Symbol *field_name = get_dumped_symbol(field_info.name_id, CHECK);
      if (field_name == vmSymbols::resolved_references_name()) {
        continue;  // Not a real field
      }
      if (fs.name() != field_name || !is_same_basic_type(fs.signature(), field_info.type)) {
        ResourceMark rm;
        THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                  err_msg("Runtime field %s %s is dumped as %s %s (class %s, dump " INT64_FORMAT ")",
                          type2name(Signature::basic_type(fs.signature())), fs.name()->as_C_string(),
                          dumped_type2name(field_info.type), field_name->as_C_string(),
                          klass->external_name(), dump.id));
      }
      fs.next();
    }

    // Skip any remaining dumped resolved references
    while (static_i < dump.static_fields.size()) {
      const HeapDumpFormat::ClassDumpRecord::Field::Info &field_info = dump.static_fields[static_i].info;
      Symbol *field_name = get_dumped_symbol(field_info.name_id, CHECK);
      if (field_name == vmSymbols::resolved_references_name()) {
        static_i++;
      } else {
        break;
      }
    }

    if (static_i < dump.static_fields.size() || instance_i < dump.instance_field_infos.size()) {
      ResourceMark rm;
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Class %s has less %s fields than its dump " INT64_FORMAT, klass->external_name(),
                        static_i < dump.static_fields.size() ? "static" : "non-static", dump.id));
    }
  }

  static bool is_same_basic_type(Symbol *signature, u1 dumped_type) {
    switch (signature->char_at(0)) {
      case JVM_SIGNATURE_CLASS:
      case JVM_SIGNATURE_ARRAY:   return dumped_type == HPROF_NORMAL_OBJECT;
      case JVM_SIGNATURE_BOOLEAN: return dumped_type == HPROF_BOOLEAN;
      case JVM_SIGNATURE_CHAR:    return dumped_type == HPROF_CHAR;
      case JVM_SIGNATURE_FLOAT:   return dumped_type == HPROF_FLOAT;
      case JVM_SIGNATURE_DOUBLE:  return dumped_type == HPROF_DOUBLE;
      case JVM_SIGNATURE_BYTE:    return dumped_type == HPROF_BYTE;
      case JVM_SIGNATURE_SHORT:   return dumped_type == HPROF_SHORT;
      case JVM_SIGNATURE_INT:     return dumped_type == HPROF_INT;
      case JVM_SIGNATURE_LONG:    return dumped_type == HPROF_LONG;
      default:                    ShouldNotReachHere(); return false;
    }
  }

  static const char *dumped_type2name(u1 type) {
    switch (type) {
      case HPROF_NORMAL_OBJECT: return "<reference type>";
      case HPROF_BOOLEAN:       return type2name(T_BOOLEAN);
      case HPROF_CHAR:          return type2name(T_CHAR);
      case HPROF_FLOAT:         return type2name(T_FLOAT);
      case HPROF_DOUBLE:        return type2name(T_DOUBLE);
      case HPROF_BYTE:          return type2name(T_BYTE);
      case HPROF_SHORT:         return type2name(T_SHORT);
      case HPROF_INT:           return type2name(T_INT);
      case HPROF_LONG:          return type2name(T_LONG);
      default:                  ShouldNotReachHere(); return nullptr;
    }
  }

  // Defines the specified class with the classfile using the provided class
  // loader and protection domain.
  static Klass *define_dumped_class(Symbol *name, const HeapDumpFormat::ClassDumpRecord &dump,
                                    instanceHandle class_loader, Handle prot_domain, TRAPS) {
    // TODO prepare the name like SystemDictionary::resolve_or_null() does, then
    // call SystemDictionary::resolve_from_stream()
    Unimplemented();
  }

  // Loads the class, initializes it by restoring its static fields, and
  // verifies names and basic types of its non-static fields.
  Klass *restore_class(const HeapDumpFormat::ClassDumpRecord &dump, TRAPS) {
    Klass **ready = _restored_classes.get(dump.id);
    if (ready != nullptr) {
      return *ready;
    }
    log_trace(restore)("Restoring class " INT64_FORMAT, dump.id);

    Klass *klass = load_class(dump, CHECK_NULL);
    _restored_classes.put_when_absent(dump.id, klass);
    _restored_classes.maybe_grow();

    // We don't set signers during the class definition like the class loaders
    // usually do, so restore and set them now
    oop class_loader = klass->class_loader();
    if (class_loader != nullptr) {
      objArrayHandle signers = restore_signers(dump.signers_id, CHECK_NULL);
      java_lang_Class::set_signers(klass->java_mirror(), signers());
    }

    if (klass->is_array_klass()) {
      // Nothing to restore for primitive array classes, and if it is an object
      // array, its bottom class should be restored individually
      if (log_is_enabled(Trace, restore)) {
        ResourceMark rm;
        log_trace(restore)("Restored class " INT64_FORMAT " (%s): array class", dump.id, klass->external_name());
      }
      return klass;
    }
    assert(klass->is_instance_klass(), "Must be");

    InstanceKlass *ik = InstanceKlass::cast(klass);

    // TODO add initialization status into the dump and use it to decide whether
    // to perform the initialization
    if (ik->is_initialized()) {
      assert(class_loader == nullptr, "Only boot-loaded classes can be pre-initialized");
      if (log_is_enabled(Trace, restore)) {
        ResourceMark rm;
        log_trace(restore)("Restored class " INT64_FORMAT " (%s): was pre-initialized", dump.id, klass->external_name());
      }
      // TODO if it is ClassLoader$ParallelLoaders, restore its loaderTypes
      // field to include all class loaders it should
      return ik;
    }

    if (dump.super_id != NULL_ID && klass->java_super() != nullptr) {
      HeapDumpFormat::ClassDumpRecord *super_dump = get_class_dump(dump.super_id, CHECK_NULL);
      Klass *super = restore_class(*super_dump, CHECK_NULL);
      if (super != klass->java_super()) {
        ResourceMark rm;
        THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                       err_msg("Class dump " INT64_FORMAT " specifies %s as its super, "
                               "but it is loaded as %s which specifies %s", dump.id,
                               super->external_name(), klass->external_name(), klass->java_super()->external_name()));
      }
    } else if (dump.super_id == NULL_ID && klass->java_super() != nullptr) {
      ResourceMark rm;
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                     err_msg("Class dump " INT64_FORMAT " specifies no super, "
                             "but it is loaded as %s which specifies %s", dump.id,
                             klass->external_name(), klass->java_super()->external_name()));
    } else if (dump.super_id != NULL_ID && klass->java_super() == nullptr) {
      ResourceMark rm;
      THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(),
                     err_msg("Class dump " INT64_FORMAT " specifies a super, "
                             "but it is loaded as %s which does not", dump.id, klass->external_name()));
    }

    ik->link_class(CHECK_NULL);
#ifdef ASSERT
    {
      MonitorLocker ml(JavaThread::current(), ik->_init_monitor);
      assert(!ik->is_being_initialized() && !ik->is_initialized(),
             "This should be the only thread performing class initialization");
      ik->set_init_state(InstanceKlass::being_initialized);
      ik->set_init_thread(JavaThread::current());
    }
#else
    ik->set_init_state(InstanceKlass::being_initialized);
    ik->set_init_thread(JavaThread::current());
#endif
    set_static_fields(ik, dump, CHECK_NULL);
    ik->set_initialization_state_and_notify(InstanceKlass::fully_initialized, JavaThread::current());

    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Restored class " INT64_FORMAT " (%s)", dump.id, klass->external_name());
    }
    return klass;
  }

  objArrayHandle restore_signers(HeapDumpFormat::id_t id, TRAPS) {
    Handle signers = restore_object(id, CHECK_({}));
    if (signers.not_null() && !signers->is_objArray()) {
      ResourceMark rm;
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Unexpected signers object type: %s", signers->klass()->external_name()), {});
    }
    return objArrayHandle(JavaThread::current(), reinterpret_cast<objArrayOop>(signers()));
  }

  // Sets static fields, basic types should have already been verified during
  // the class loading.
  void set_static_fields(InstanceKlass *ik, const HeapDumpFormat::ClassDumpRecord &dump, TRAPS) {
    FieldStream fs(ik, true, true);
    size_t static_i = 0;

    while (!fs.eos() && static_i < dump.static_fields.size()) {
      if (!fs.access_flags().is_static()) {
        fs.next();
        continue;
      }

      const HeapDumpFormat::ClassDumpRecord::Field &field = dump.static_fields[static_i++];

      Symbol *field_name = get_dumped_symbol(field.info.name_id, CHECK);
      if (field_name == vmSymbols::resolved_references_name()) {
        // TODO restore and apply the resolved references?
        continue;
      }

      precond(fs.name() == field_name && is_same_basic_type(fs.signature(), field.info.type));
      set_field(ik->java_mirror(), fs, field.value, CHECK);

      fs.next();
    }

    // Process any remaining dumped resolved references
    while (static_i < dump.static_fields.size()) {
      const HeapDumpFormat::ClassDumpRecord::Field::Info &field_info = dump.static_fields[static_i].info;
      Symbol *field_name = get_dumped_symbol(field_info.name_id, CHECK);
      if (field_name == vmSymbols::resolved_references_name()) {
        // TODO restore and apply the resolved references?
        static_i++;
      } else {
        break;
      }
    }

    postcond(static_i == dump.static_fields.size());
#ifdef ASSERT
    for (; !fs.eos(); fs.next()) { postcond(!fs.access_flags().is_static()); }
#endif
  }

  void set_field(oop obj, const FieldStream &fs, const HeapDumpFormat::BasicValue &val, TRAPS) {
    switch (fs.signature()->char_at(0)) {
      case JVM_SIGNATURE_CLASS:
      case JVM_SIGNATURE_ARRAY: {
        Handle restored = restore_object(val.as_object_id, CHECK);
        // Only basic type has been validated until now, so validate the class
        if (restored.not_null() && !restored->klass()->is_subtype_of(get_field_class(fs))) {
          ResourceMark rm;
          THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Field referencing a %s is dumped as an incompatible %s instance",
                            get_field_class(fs)->external_name(), restored->klass()->external_name()));
        }
        obj->obj_field_put(fs.offset(), restored());
        break;
      }
      case JVM_SIGNATURE_BOOLEAN: obj->bool_field_put(fs.offset(), val.as_boolean);  break;
      case JVM_SIGNATURE_CHAR:    obj->char_field_put(fs.offset(), val.as_char);     break;
      case JVM_SIGNATURE_FLOAT:   obj->float_field_put(fs.offset(), val.as_float);   break;
      case JVM_SIGNATURE_DOUBLE:  obj->double_field_put(fs.offset(), val.as_double); break;
      case JVM_SIGNATURE_BYTE:    obj->byte_field_put(fs.offset(), val.as_byte);     break;
      case JVM_SIGNATURE_SHORT:   obj->short_field_put(fs.offset(), val.as_short);   break;
      case JVM_SIGNATURE_INT:     obj->int_field_put(fs.offset(), val.as_int);       break;
      case JVM_SIGNATURE_LONG:    obj->long_field_put(fs.offset(), val.as_long);     break;
      default:                    ShouldNotReachHere();
    }
  }

  static Klass *get_field_class(const FieldStream &fs) {
    JavaThread *current = JavaThread::current();
    InstanceKlass *field_holder = fs.field_descriptor().field_holder();
    Handle loader = Handle(current, field_holder->class_loader());

    // Strip the envelope, if any
    TempNewSymbol class_name = fs.signature();
    if (Signature::has_envelope(class_name)) {
      ResourceMark rm;
      class_name = SymbolTable::new_symbol(class_name->as_C_string() + 1, class_name->utf8_length() - 2);
    }

    // TODO after dictionary restoration for initiating loaders is implemented,
    // use SystemDictionary::find_instance_or_array_klass() -- it should
    // produce the same results then
    Klass *field_class = SystemDictionary::find_constrained_instance_or_array_klass(current, class_name, loader);
#ifdef ASSERT
    {
      ResourceMark rm;
      assert(field_class != nullptr, "Cannot find field class: field %s with signature %s, holder class %s",
             fs.name()->as_C_string(), fs.signature()->as_C_string(), field_holder->external_name());
    }
#endif

    return field_class;
  }

  Handle restore_object(HeapDumpFormat::id_t id, TRAPS) {
    if (id == NULL_ID) {
      return Handle();
    }
    Handle *ready = _restored_objects.get(id);
    if (ready != nullptr) {
      return *ready;
    }

    HeapDumpFormat::InstanceDumpRecord  *instance_dump   = _heap_dump.instance_dump_records.get(id);
    HeapDumpFormat::ObjArrayDumpRecord  *obj_array_dump  = _heap_dump.obj_array_dump_records.get(id);
    HeapDumpFormat::PrimArrayDumpRecord *prim_array_dump = _heap_dump.prim_array_dump_records.get(id);
    // HeapDumper does not include Class<*> instances of non-primitive classes
    // in the instance dumps
    HeapDumpFormat::ClassDumpRecord     *class_dump =      _heap_dump.class_dump_records.get(id);

    if (instance_dump != nullptr && obj_array_dump == nullptr && prim_array_dump == nullptr && class_dump == nullptr) {
      Handle handle = restore_instance(*instance_dump, CHECK_NH);
      return handle;
    }
    if (instance_dump == nullptr && obj_array_dump != nullptr && prim_array_dump == nullptr && class_dump == nullptr) {
      Handle handle = restore_obj_array(*obj_array_dump, CHECK_NH);
      return handle;
    }
    if (instance_dump == nullptr && obj_array_dump == nullptr && prim_array_dump != nullptr && class_dump == nullptr) {
      Handle handle = restore_prim_array(*prim_array_dump, CHECK_NH);
      return handle;
    }
    if (instance_dump == nullptr && obj_array_dump == nullptr && prim_array_dump == nullptr && class_dump != nullptr) {
      Klass *klass = restore_class(*class_dump, CHECK_NH);
      return Handle(JavaThread::current(), klass->java_mirror());
    }

    THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
               err_msg("Object dump " INT64_FORMAT " occurs in none or multiple dump categories", id), {});
  }

  instanceHandle restore_instance(const HeapDumpFormat::InstanceDumpRecord &dump, TRAPS) {
    assert(!_restored_objects.contains(dump.id), "Use restore_object() which also checks for ID duplication");
    log_trace(restore)("Restoring instance " INT64_FORMAT, dump.id);

    HeapDumpFormat::ClassDumpRecord *class_dump = get_class_dump(dump.class_id, CHECK_({}));
    InstanceKlass *klass = load_instance_class(*class_dump, false, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      Handle e(Thread::current(), PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      THROW_MSG_CAUSE_(vmSymbols::java_lang_IllegalArgumentException(),
                       err_msg("Cannot load class of instance dump " INT64_FORMAT, dump.id), e, {});
    }

    // HeapDumper creates Class<*> instance dumps for primitive types
    if (klass == vmClasses::Class_klass()) {
      return get_primitive_class_mirror(*class_dump, dump, THREAD);
    }

    instanceHandle handle;
    if (klass->is_class_loader_instance_klass()) {
      instanceHandle *ready = _prepared_class_loaders.get(dump.id);
      if (ready != nullptr) {
        assert(ready->not_null(), "Stub leak");
        handle = *ready;
      } else {
        handle = prepare_class_loader(klass, dump, CHECK_({}));
        _prepared_class_loaders.put_when_absent(dump.id, handle);
      }
    } else if (klass->is_subclass_of(vmClasses::ProtectionDomain_klass())) {
      instanceHandle *ready = _allocated_prot_domains.get(dump.id);
      if (ready != nullptr) {
        assert(ready->not_null(), "Stub leak");
        handle = *ready;
      } else {
        handle = klass->allocate_instance_handle(CHECK_({}));
        _prepared_class_loaders.put_when_absent(dump.id, handle);
      }
    } else {
      handle = klass->allocate_instance_handle(CHECK_({}));
    }

    assert(!_restored_objects.contains(dump.id), "Should still be not restored");
    _restored_objects.put_when_absent(dump.id, handle);

    restore_class(*class_dump, CHECK_({}));

    // TODO also treat classes like java.lang.Thread specially
    if (klass->is_class_loader_instance_klass()) {
      // TODO if this not a platform/app class loader restored by CDS, restore
      // all but the prepared fields, treating classes field specially
    } else {
      set_instance_fields(handle, dump, CHECK_({}));
    }

    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Restored instance " INT64_FORMAT " (%s)", dump.id, klass->external_name());
    }
    return handle;
  }

  instanceHandle get_primitive_class_mirror(const HeapDumpFormat::ClassDumpRecord &class_dump,
                                            const HeapDumpFormat::InstanceDumpRecord &instance_dump, TRAPS) {
    // We rely on j.l.Class name field to reveal the primitive type
#ifdef ASSERT
    {
      fieldDescriptor fd;
      precond(vmClasses::Class_klass()->find_local_field(vmSymbols::name_name(), vmSymbols::string_signature(), &fd));
    }
#endif  // ASSERT

    size_t name_field_offset = 0;
    for (size_t i = 0; i < class_dump.instance_field_infos.size(); i++) {
      const HeapDumpFormat::ClassDumpRecord::Field::Info &field_info = class_dump.instance_field_infos[i];
      if (field_info.type != HPROF_NORMAL_OBJECT) {
        assert(HeapDumpFormat::prim2size(field_info.type) != 0, "Must be a primitive type");
        name_field_offset += HeapDumpFormat::prim2size(field_info.type);
        continue;
      }
      Symbol *field_name = get_dumped_symbol(field_info.name_id, CHECK_({}));
      if (field_name != vmSymbols::name_name()) {
        name_field_offset += _heap_dump.id_size;
        continue;
      }
      break;
    }
    if (name_field_offset >= instance_dump.fields_data.size()) {
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Incorrect class instance dump " INT64_FORMAT ": no name field", instance_dump.id), {});
    }

    HeapDumpFormat::BasicValue name_id;
    if (instance_dump.read_field(name_field_offset, JVM_SIGNATURE_CLASS, _heap_dump.id_size, &name_id) == 0) {
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Unexpected fields data size in class instance dump " INT64_FORMAT, instance_dump.id), {});
    }

    Handle name = restore_object(name_id.as_object_id, CHECK_({}));
    if (name.is_null()) {
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Name field of class instance dump " INT64_FORMAT " is uninitialized", instance_dump.id), {});
    }
    assert(name->klass() == vmClasses::String_klass(), "This must be checked during the field verification");

    ResourceMark rm;
    char *name_str = java_lang_String::as_quoted_ascii(name());

    BasicType type = name2type(name_str);
    if (!is_java_primitive(type) && type != T_VOID) {
      THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                 err_msg("Only classes of primitive types can be instance-dumped, "
                         "but found class instance dump named %s", name_str), {});
    }

    instanceHandle handle(Thread::current(), reinterpret_cast<instanceOop>(Universe::java_mirror(type)));
    assert(handle.not_null(), "Primitive class mirror must not be null");
    return handle;
  }

  // Sets non-static fields, basic types should have already been verified
  // during the class loading.
  void set_instance_fields(instanceHandle handle, const HeapDumpFormat::InstanceDumpRecord &dump, TRAPS) {
    precond(handle.not_null());

    FieldStream fs(InstanceKlass::cast(handle->klass()), false, false);
    size_t dump_offset = 0;

    for (; !fs.eos() && dump_offset < dump.fields_data.size(); fs.next()) {
      if (fs.access_flags().is_static()) {
        continue;
      }

      HeapDumpFormat::BasicValue value;
      size_t bytes_read = dump.read_field(dump_offset, fs.signature()->char_at(0), _heap_dump.id_size, &value);
      if (bytes_read == 0) {  // Reading violates dumped fields array bounds
        break;
      }
      set_field(handle(), fs, value, CHECK);

      dump_offset += bytes_read;
    }

    // Skip any remaining static fields
    while (!fs.eos() && fs.access_flags().is_static()) {
      fs.next();
    }

    if (!fs.eos() || dump_offset < dump.fields_data.size()) {
      THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                err_msg("Unexpected fields data size in instance dump " INT64_FORMAT, dump.id));
    }
  }

  objArrayHandle restore_obj_array(const HeapDumpFormat::ObjArrayDumpRecord &dump, TRAPS) {
    assert(!_restored_objects.contains(dump.id), "Use restore_object() which also checks for ID duplication");
    log_trace(restore)("Restoring object array " INT64_FORMAT, dump.id);

    HeapDumpFormat::ClassDumpRecord *class_dump = get_class_dump(dump.array_class_id, CHECK_({}));
    ObjArrayKlass *klass;
    {
      Klass *k = load_class(*class_dump, CHECK_({}));
      if (!k->is_objArray_klass()) {
        ResourceMark rm;
        THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("Object array dump " INT64_FORMAT " has illegal class %s", dump.id, k->external_name()),
                   {});
      }
      klass = ObjArrayKlass::cast(k);
    }

    objArrayHandle handle;
    {
      objArrayOop o = klass->allocate(dump.elem_ids.size(), CHECK_({}));
      handle = objArrayHandle(JavaThread::current(), o);
    }

    assert(!_restored_objects.contains(dump.id), "Should still be not restored");
    _restored_objects.put_when_absent(dump.id, handle);

    restore_class(*class_dump, CHECK_({}));

    for (size_t i = 0; i < dump.elem_ids.size(); i++) {
      Handle elem = restore_object(dump.elem_ids[i], CHECK_({}));
      if (elem.is_null() || elem->klass()->is_subtype_of(klass->element_klass())) {
        handle->obj_at_put(i, elem());
      } else {
        ResourceMark rm;
        THROW_MSG_(vmSymbols::java_lang_IllegalArgumentException(),
                   err_msg("%s array has element %zu of incompatible type %s in the dump",
                           klass->element_klass()->external_name(), i, elem->klass()->external_name()),
                   {});
      }
    }

    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Restored object array " INT64_FORMAT " (%s)", dump.id, klass->external_name());
    }
    return handle;
  }

  typeArrayHandle restore_prim_array(const HeapDumpFormat::PrimArrayDumpRecord &dump, TRAPS) {
    assert(!_restored_objects.contains(dump.id), "Use restore_object() which also checks for ID duplication");
    log_trace(restore)("Restoring primitive array " INT64_FORMAT, dump.id);

    typeArrayOop o;
    switch (dump.elem_type) {
      case HPROF_BOOLEAN:
        o = oopFactory::new_typeArray_nozero(T_BOOLEAN, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jboolean) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->bool_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_CHAR:
        o = oopFactory::new_typeArray_nozero(T_CHAR, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jchar) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->char_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_FLOAT:
        o = oopFactory::new_typeArray_nozero(T_FLOAT, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jfloat) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->float_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_DOUBLE:
        o = oopFactory::new_typeArray_nozero(T_DOUBLE, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jdouble) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->double_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_BYTE:
        o = oopFactory::new_typeArray_nozero(T_BYTE, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jbyte) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->byte_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_SHORT:
        o = oopFactory::new_typeArray_nozero(T_SHORT, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jshort) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->short_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_INT:
        o = oopFactory::new_typeArray_nozero(T_INT, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jint) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->int_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      case HPROF_LONG:
        o = oopFactory::new_typeArray_nozero(T_LONG, dump.elems_num, CHECK_({}));
        precond(dump.elems_num * sizeof(jlong) == dump.elems_data.size());
        if (dump.elems_num > 0) memcpy(o->long_at_addr(0), dump.elems_data.mem(), dump.elems_data.size());
        break;
      default:
        ShouldNotReachHere();  // Ensured by the parser
    }

    typeArrayHandle handle(JavaThread::current(), o);
    _restored_objects.put_when_absent(dump.id, handle);
    _restored_objects.maybe_grow();
    if (log_is_enabled(Trace, restore)) {
      ResourceMark rm;
      log_trace(restore)("Restored primitive array " INT64_FORMAT " (%s)", dump.id, handle->klass()->external_name());
    }
    return handle;
  }
};

// Restore in portable mode.
void crac::restore_portable(TRAPS) {
  assert(is_portable_mode(), "Use crac::restore instead");
  precond(CRaCRestoreFrom != nullptr);

  char path[JVM_MAXPATHLEN];
  os::snprintf_checked(path, sizeof(path), "%s%s%s", CRaCRestoreFrom, os::file_separator(), PMODE_HEAP_DUMP_FILENAME);

  ParsedHeapDump heap_dump;
  const char* err_str = HeapDumpParser::parse(path, &heap_dump);
  if (err_str != nullptr) {
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
              err_msg("Restore failed: cannot parse heap dump %s (%s)", path, err_str));
  }

  HeapRestorer heap_restorer(heap_dump);
  heap_restorer.restore_heap(THREAD);
  if (HAS_PENDING_EXCEPTION) {
    Handle e(Thread::current(), PENDING_EXCEPTION);
    CLEAR_PENDING_EXCEPTION;
    THROW_MSG_CAUSE(vmSymbols::java_lang_IllegalArgumentException(), err_msg("Restore failed: cannot restore heap"), e);
  }
}
