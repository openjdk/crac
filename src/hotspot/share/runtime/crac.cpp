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

#include "code/codeCache.hpp"
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
#ifdef LINUX
#include "os_linux.hpp"
#endif

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

  // We don't invoke this inside memory_checkpoint() for symmetry;
  // CodeCache must be restored earlier (see below)
  if (CRPersistMemory && !CodeCache::persist_for_checkpoint()) {
    memory_restore();
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

  if (CRPersistMemory) {
    // CodeCache must be restored before VM_Version::crac_restore as this
    // generates some code in the code cache on the fly
    CodeCache::load_on_restore();
  }

  // It needs to check CPU features before any other code (such as VM_Crac::read_shm) depends on them.
  VM_Version::crac_restore();

  if (shmid <= 0 || !VM_Crac::read_shm(shmid)) {
    _restore_start_time = os::javaTimeMillis();
    _restore_start_nanos = os::javaTimeNanos();
  } else {
    _restore_start_nanos += crac::monotonic_time_offset();
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

struct persisted_mem_header {
  const int32_t version;
  const size_t slots;
  char type[16];
};

static bool write_fully(int fd, const char* buf, size_t len) {
  do {
    int n = os::write(fd, buf, len);
    if (n < 0) {
      if (errno != EINTR) {
        fprintf(stderr, "Addr %p, length %lx\n", buf, len);
        perror("Cannot write");
        return false;
      }
    } else {
      buf += n;
      len -= n;
    }
  }
  while (len > 0);
  return true;
}

static bool read_fully(int fd, char *dest, size_t n) {
  size_t rd = 0;
  do {
    ssize_t r = os::read(fd, dest + rd, n - rd);
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


crac::MemoryPersisterBase::MemoryPersisterBase(size_t slots): _fd(-1), _slots(slots), _index_curr(0) {
  _index_begin = sizeof(persisted_mem_header);
  size_t page_size = os::vm_page_size();
  // end of index/begin of data must be aligned to page size
  _index_end = (((_index_begin + slots * sizeof(struct record) - 1) & ~(page_size - 1)) + 1) * page_size;
  _offset_curr = _index_end;
  _index = NEW_C_HEAP_ARRAY(struct record, slots, mtInternal);
}

crac::MemoryPersisterBase::~MemoryPersisterBase() {
  FREE_C_HEAP_ARRAY(struct record, _index);
  if (_fd < 0) {
    return;
  }
  os::close(_fd);
}

crac::MemoryPersister::~MemoryPersister() {
  if (_fd < 0) {
    return;
  }
  os::seek_to_file_offset(_fd, _index_begin);
  write_fully(_fd, (char *) _index, _index_curr * sizeof(struct record));
}

bool crac::MemoryPersisterBase::open(bool loading, const char *filename) {
  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s%s%s", CRaCCheckpointTo, os::file_separator(), filename);
  _fd = os::open(path, loading ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC), S_IRUSR | S_IWUSR);
  if (_fd < 0) {
    perror("Cannot open persisted memory file");
    return false;
  }
  return true;
}

bool crac::MemoryPersister::open(const char *filename, const char type[16]) {
  if (!MemoryPersisterBase::open(false, filename)) {
    return false;
  }
  struct persisted_mem_header header = {
    .version = 1,
    .slots = _slots
  };
  memcpy(header.type, type, 16);
  if (!write_fully(_fd, (char *) &header, sizeof(header))) {
    tty->print_cr("Cannot write persisted memory file header");
    return false;
  }
  os::seek_to_file_offset(_fd, _index_end);
  return true;
}

bool crac::MemoryPersister::store(void *addr, size_t length, size_t mapped_length) {
  if (mapped_length == 0) {
    return true;
  }

  size_t page_size = os::vm_page_size();
  guarantee(_index_curr < _slots, "No more index space reserved");
  assert(((u_int64_t) addr & (page_size - 1)) == 0, "Unaligned address %p", addr);
  assert(length <= mapped_length, "Useful length %lx shorter than mapped %lx", length, mapped_length);
  assert((mapped_length & (page_size - 1)) == 0, "Unaligned length %lx", length);

  if (length > 0 && !write_fully(_fd, (char *) addr, length)) {
    tty->print_cr("Cannot store persisted memory");
    return false;
  }
  size_t aligned_length = align_up(length, page_size);
  _index[_index_curr++] = {
    .addr = (u_int64_t) addr,
    .length = (u_int64_t) length,
    .offset = (u_int64_t) _offset_curr
  };
  _offset_curr += aligned_length;
  if (length != aligned_length) {
    os::seek_to_file_offset(_fd, _offset_curr);
  }
  return unmap(addr, mapped_length);
}

bool crac::MemoryPersister::store_gap(void *addr, size_t length) {
  assert(((u_int64_t) addr & (os::vm_page_size() - 1)) == 0, "Unaligned address");
  assert((length & (os::vm_page_size() - 1)) == 0, "Unaligned length");
  if (length == 0) {
    return true;
  }
  // Not storing anything, not even to index
  return unmap(addr, length);
}

bool crac::MemoryLoader::open(const char *filename, const char type[16]) {
  if (!MemoryPersisterBase::open(true, filename)) {
    return false;
  }
  struct persisted_mem_header header = {};
  if (!read_fully(_fd, (char *) &header, sizeof(header))) {
    tty->print_cr("Cannot read persisted memory file header");
    return false;
  } else if (header.version != 1) {
    tty->print_cr("Invalid persisted memory file version");
    return false;
  } else if (header.slots != _slots) {
    tty->print_cr("Number of persisted memory file slots does not match");
    return false;
  } else if (memcmp(header.type, type, 16)) {
    tty->print_cr("Mismatch for type of persisted memory file");
    return false;
  }

  if (!read_fully(_fd, (char *) _index, _slots * sizeof(struct record))) {
    tty->print_cr("Cannot read persisted memory file index");
    return false;
  }

  return true;
}

bool crac::MemoryLoader::load(void *addr, size_t expected_length, size_t mapped_length, bool executable) {
  if (mapped_length == 0) {
    return true;
  }

  size_t at = UINT_MAX;
  // we're not going from 0 to optimize reading in the same order as writing
  for (size_t i = _index_curr; i < _slots; ++i) {
    if (_index[i].addr == (u_int64_t) addr) {
      at = i;
      break;
    }
  }
  if (at != UINT_MAX) {
    for (size_t i = 0; i < _index_curr; ++i) {
      if (_index[i].addr == (u_int64_t) addr) {
        at = i;
        break;
      }
    }
  }
  if (at == UINT_MAX) {
    tty->print_cr("Cannot find region with address %p", addr);
    return false;
  }
  if (_index[at].length != (u_int64_t) expected_length) {
    tty->print_cr("Persisted memory region length does not match at %p: %lu vs. %lu",
    addr, expected_length, _index[at].length);
    return false;
  }
  size_t offset = _index[at].offset;
  size_t aligned_length = align_up(expected_length, os::vm_page_size());
  if (expected_length > 0 && !map(addr, expected_length, _fd, offset, executable)) {
    return false;
  }
  if (aligned_length < mapped_length && !map((char *) addr + aligned_length, mapped_length - aligned_length, -1, 0, executable)) {
    return false;
  }
  _index_curr = at + 1;
  return true;
}
