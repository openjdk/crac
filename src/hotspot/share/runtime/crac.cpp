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

#include "classfile/classLoader.hpp"
#include "compiler/compileBroker.hpp"
#include "jfr/jfr.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/allocation.hpp"
#include "memory/oopFactory.hpp"
#include "nmt/memTag.hpp"
#include "oops/oopCast.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/crac_engine.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/crac.hpp"
#include "runtime/globals.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "services/classLoadingService.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/debug.hpp"
#include "utilities/decoder.hpp"
#include "os.inline.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"

static jlong _restore_start_time;
static jlong _restore_start_nanos;

CracEngine *crac::_engine = nullptr;
unsigned int crac::_generation = 1;
char crac::_checkpoint_bootid[UUID_LENGTH];
jlong crac::_checkpoint_wallclock_seconds;
jlong crac::_checkpoint_wallclock_nanos;
jlong crac::_checkpoint_monotonic_nanos;
jlong crac::_javaTimeNanos_offset;

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

void VM_Crac::print_resources(const char* msg, ...) {
  if (CRaCPrintResourcesOnCheckpoint) {
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

static int append_time(char *buf, size_t buflen, bool iso8601, bool zero_pad, int width, jlong timeMillis) {
  if (iso8601) {
    if (width >= 0 || zero_pad) {
      log_warning(crac)("Cannot use zero-padding or set width for ISO-8601 time in CRaCCheckpointTo=%s", CRaCCheckpointTo);
    }
    // os::iso8601_time formats with dashes and colons, we want the basic version
    time_t time = timeMillis / 1000;
    struct tm tms;
    if (os::gmtime_pd(&time, &tms) == nullptr) {
      log_warning(crac)("Cannot format time " JLONG_FORMAT, timeMillis);
      return -1;
    }
    return (int) strftime(buf, buflen, "%Y%m%dT%H%M%SZ", &tms);
  } else {
    // width -1 works too (means 1 char left aligned => we always print at least 1 char)
    return snprintf(buf, buflen, zero_pad ? "%0*" PRId64 : "%*" PRId64, width, (int64_t) (timeMillis / 1000));
  }
}

static int append_size(char *buf, size_t buflen, bool zero_pad, int width, size_t size) {
  if (zero_pad) {
    return snprintf(buf, buflen, "%0*zu", width, size);
  } else if (width >= 0) {
    return snprintf(buf, buflen, "%*zu", width, size);
  } else {
    static constexpr const char *suffixes[] = { "k", "M", "G" };
    const char *suffix = "";
    for (size_t i = 0; i < ARRAY_SIZE(suffixes) && size != 0 && (size & 1023) == 0; ++i) {
      suffix = suffixes[i];
      size = size >> 10;
    }
    return snprintf(buf, buflen, "%zu%s", size, suffix);
  }
}

#define check_no_width_padding() do { \
    if (width >= 0) { \
      log_warning(crac)("Cannot set width for %%%c in CRaCCheckpointTo=%s", c, CRaCCheckpointTo); \
    } \
    if (zero_pad) { \
      log_warning(crac)("Cannot use zero-padding for %%%c in CRaCCheckpointTo=%s", c, CRaCCheckpointTo); \
    } \
  } while (false)

#define check_retval(statement) do { \
    int ret = statement; \
    if ((size_t) ret > buflen) { \
      log_error(crac)("Error interpolating CRaCCheckpointTo=%s (too long)", CRaCCheckpointTo); \
      return false; \
    } else if (ret < 0) { \
      log_error(crac)("Error interpolating CRaCCheckpointTo=%s", CRaCCheckpointTo); \
      return false; \
    } \
    buf += ret; \
    buflen -= ret; \
  } while (false)

static inline jlong boot_time() {
  // RuntimeMxBean.getStartTime() returns Management::vm_init_done_time() but this is not initialized
  // when CRaC checks the boot time early in the initialization phase
  return os::javaTimeMillis() - (1000 * os::elapsed_counter() / os::elapsed_frequency());
}

bool crac::interpolate_checkpoint_location(char *buf, size_t buflen, bool *fixed) {
  *fixed = true;
  for (size_t si = 0; ; si++) {
    if (buflen == 0) {
      log_error(crac)("Error interpolating CRaCCheckpointTo=%s (too long)", CRaCCheckpointTo);
      return false;
    }
    char c = CRaCCheckpointTo[si];
    if (c != '%') {
      *(buf++) = c;
      --buflen;
      if (!c) {
        break;
      } else {
        continue;
      }
    }

    si++;
    c = CRaCCheckpointTo[si];
    bool zero_pad = false;
    if (c == '0') {
      zero_pad = true;
      si++;
    }
    size_t width_start = si;
    while (CRaCCheckpointTo[si] >= '0' && CRaCCheckpointTo[si] <= '9') {
      ++si;
    }
    if (zero_pad && width_start == si) {
      log_error(crac)("CRaCCheckpointTo=%s contains a pattern with zero padding but no length", CRaCCheckpointTo);
      return false;
    }
    const int width = si > width_start ? atoi(&CRaCCheckpointTo[width_start]) : -1;
    c = CRaCCheckpointTo[si];
    switch (c) {
    case '%':
      check_no_width_padding();
      *(buf++) = '%';
      --buflen;
      break;
    case 'a': // CPU architecture; matches system property "os.arch"
        check_no_width_padding();
#ifndef ARCHPROPNAME
# error "ARCHPROPNAME must be defined by build scripts"
#endif
      check_retval(snprintf(buf, buflen, "%s", ARCHPROPNAME));
      break;
    case 'f': { // CPU features
        check_no_width_padding();
        VM_Version::VM_Features data;
        if (VM_Version::cpu_features_binary(&data)) {
          check_retval(data.print_numbers(buf, buflen, true));
        } // otherwise just empty string
      }
      break;
    case 'u': { // Random UUID (v4)
        u4 uuid[4];
        if (!random_bytes(reinterpret_cast<u1 *>(uuid), sizeof(uuid))) {
          log_error(crac)("Cannot generate random UUID");
          return false;
        }
        check_no_width_padding();
        *fixed = false; // FIXME?
        u4 time_mid_high = uuid[0];
        u4 seq_and_node_low = uuid[1];
        check_retval(snprintf(buf, buflen, "%08x-%04x-4%03x-%04x-%04x%08x",
          uuid[2], time_mid_high >> 16, time_mid_high & 0xFFF,
          0x8000 | (seq_and_node_low & 0x3FFF), seq_and_node_low >> 16, uuid[3]));
      }
      break;
    case 't': // checkpoint (current) time
    case 'T':
      *fixed = false;
      check_retval(append_time(buf, buflen, c == 't', zero_pad, width, os::javaTimeMillis()));
      break;
    case 'b': // boot time
    case 'B':
      check_retval(append_time(buf, buflen, c == 'b', zero_pad, width, boot_time()));
      break;
    case 'r': // last restore time
    case 'R':
      check_retval(append_time(buf, buflen, c == 'r', zero_pad, width, _generation != 1 ? crac::restore_start_time() : boot_time()));
      break;
    case 'p': // PID
      check_retval(snprintf(buf, buflen, zero_pad ? "%0*d" : "%*d", width, os::current_process_id()));
      break;
    case 'c': // Number of CPUs
      check_retval(snprintf(buf, buflen, zero_pad ? "%0*d" : "%*d", width, os::active_processor_count()));
      break;
    case 'm': // Max heap size
      *fixed = false; // Heap size is not yet resolved when this is called from prepare_checkpoint()
      check_retval(append_size(buf, buflen, zero_pad, width, Universe::heap() != nullptr ? Universe::heap()->max_capacity() : 0));
      break;
    case 'g': // CRaC generation
      check_retval(snprintf(buf, buflen, zero_pad ? "%0*d" : "%*d", width, _generation));
      break;
    default: /* incl. terminating '\0' */
      log_error(crac)("CRaCCheckpointTo=%s contains an invalid pattern", CRaCCheckpointTo);
      return false;
    }
  }
  return true;
}

#undef check_no_width_padding
#undef check_retval

static bool ensure_checkpoint_dir(const char *path, bool rm) {
  struct stat st;
  if (0 == os::stat(path, &st)) {
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
      log_error(crac)("CRaCCheckpointTo=%s is not a directory", path);
      return false;
    }
  } else {
    if (-1 == os::mkdir(path)) {
      log_error(crac)("Cannot create CRaCCheckpointTo=%s: %s", path, os::strerror(errno));
      return false;
    }
    if (rm && -1 == os::rmdir(path)) {
      log_warning(crac)("Cannot cleanup after CRaCCheckpointTo check: %s", os::strerror(errno));
      // not fatal
    }
  }
  return true;
}

#ifndef PATH_MAX
# define PATH_MAX 1024
#endif

int crac::checkpoint_restore(int *shmid) {
  guarantee(_engine != nullptr, "CRaC engine is not initialized");

  crac::record_time_before_checkpoint();

  // CRaCCheckpointTo can be changed on restore, and if this contains a pattern
  // it might not have been configured => we need to update the conf.
  // Note that CRaCEngine and CRaCEngineOptions are not updated (as documented)
  // so we don't need to re-init the whole engine handle.
  char image_location[PATH_MAX];
  bool ignored;
  if (!interpolate_checkpoint_location(image_location, sizeof(image_location), &ignored) ||
      !ensure_checkpoint_dir(image_location, false) ||
      !_engine->configure_image_location(image_location)) {
    return JVM_CHECKPOINT_ERROR;
  }

  // Setup CPU arch & features only during the first checkpoint; the feature set
  // cannot change after initial boot (and we don't support switching the engine).
  if (_generation == 1 && !VM_Version::ignore_cpu_features()) {
    VM_Version::VM_Features current_features;
    if (VM_Version::cpu_features_binary(&current_features)) {
      switch (_engine->prepare_image_constraints_api()) {
        case CracEngine::ApiStatus::OK:
          if (!_engine->store_cpuinfo(&current_features)) {
            return JVM_CHECKPOINT_ERROR;
          }
          break;
        case CracEngine::ApiStatus::ERR:
          return JVM_CHECKPOINT_ERROR;
        case CracEngine::ApiStatus::UNSUPPORTED:
          log_warning(crac)("Cannot store CPUFeatures for checkpoint "
            "with the selected CRaC engine");
          break;
      }
    }
  }

  const int ret = _engine->checkpoint();
  if (ret != 0) {
    log_error(crac)("CRaC engine failed to checkpoint to %s: error %i", image_location, ret);
    return JVM_CHECKPOINT_ERROR;
  }

  switch (_engine->prepare_restore_data_api()) {
    case CracEngine::ApiStatus::OK: {
      constexpr size_t required_size = sizeof(*shmid);
      const size_t available_size = _engine->get_restore_data(shmid, sizeof(*shmid));
      if (available_size == 0) { // Possible if we were not killed by the engine and thus there is no restoring JVM
        *shmid = 0; // Not an error, just no restore data
        break;
      }
      if (available_size == required_size) {
        break;
      }
      if (available_size > required_size) {
        log_debug(crac)("CRaC engine has more restore data than expected");
        break;
      }
      log_error(crac)("CRaC engine provided not enough restore data: need %zu bytes, got %zu",
                      required_size, available_size);
      // fallthrough
    }
    case CracEngine::ApiStatus::ERR:         *shmid = -1; break; // Indicates error to the caller
    case CracEngine::ApiStatus::UNSUPPORTED: *shmid = 0;  break; // Not an error, just no restore data
  }

#ifdef LINUX
  if (CRaCCPUCountInit) {
    os::Linux::initialize_cpu_count();
  }
#endif //LINUX

  crac::update_javaTimeNanos_offset();

  if (CRaCTraceStartupTime) {
    tty->print_cr("STARTUPTIME " JLONG_FORMAT " restore-native", os::javaTimeNanos());
  }

  return JVM_CHECKPOINT_OK;
}

bool VM_Crac::read_shm(int shmid) {
  precond(shmid > 0);
  CracSHM shm(shmid);
  int shmfd = shm.open(O_RDONLY);
  shm.unlink();
  if (shmfd < 0) {
    log_error(crac)("Cannot read restore parameters");
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

class DefaultStreamHandler {
public:
  DefaultStreamHandler() {
    defaultStream::instance->before_checkpoint();
  }

  ~DefaultStreamHandler() {
    defaultStream::instance->after_restore();
  }
};

void VM_Crac::doit() {
  // dry-run fails checkpoint
  bool ok = true;
  DefaultStreamHandler defStreamHandler;

  Decoder::before_checkpoint();
  if (!check_fds()) {
    ok = false;
  }

  if ((!ok || _dry_run) && CRaCHeapDumpOnCheckpointException) {
    HeapDumper::dump_heap();
  }

  if (!ok && CRaCPauseOnCheckpointError) {
    os::message_box("Checkpoint failed", "Errors were found during checkpoint.");
  }

  if (!ok || _dry_run) {
    _ok = ok;
    return;
  }

  if (!memory_checkpoint()) {
    return;
  }

  int shmid = -1;
  if (CRaCSkipCheckpoint) {
    log_info(crac)("Skip Checkpoint");
    shmid = 0;
  } else {
    log_info(crac)("Checkpoint ...");
    report_ok_to_jcmd_if_any();
    int ret = crac::checkpoint_restore(&shmid);
    if (ret == JVM_CHECKPOINT_ERROR) {
      memory_restore();
      return;
    }
  }

  crac::_generation++;
  Arguments::reset_for_crac_restore();
  os::reset_cached_process_id();

  if (shmid == 0) { // E.g. engine does not support restore data
    log_debug(crac)("Restore parameters (JVM flags, env vars, system properties, arguments...) not provided");
    _restore_start_time = os::javaTimeMillis();
    _restore_start_nanos = os::javaTimeNanos();
  } else {
    if (shmid < 0 || !VM_Crac::read_shm(shmid)) {
      vm_direct_exit(1, "Restore cannot continue, VM will exit."); // More info in logs
      ShouldNotReachHere();
    }
    _restore_start_nanos += crac::monotonic_time_offset();
  }

  if (CRaCResetStartTime) {
    crac::reset_time_counters();
  }

  memory_restore();

  wakeup_threads_in_timedwait_vm();

  _ok = true;
}

void crac::print_engine_info_and_exit(const char *pattern) {
  CracEngine engine;
  if (!engine.is_initialized()) {
    return;
  }

  const CracEngine::ApiStatus status = engine.prepare_description_api();
  if (status == CracEngine::ApiStatus::ERR) {
    return;
  }
  if (status == CracEngine::ApiStatus::UNSUPPORTED) {
    tty->print_cr("Selected CRaC engine does not provide information about itself");
    vm_exit(0);
    ShouldNotReachHere();
  }
  postcond(status == CracEngine::ApiStatus::OK);

  const char *description = engine.description();
  if (description == nullptr) {
    log_error(crac)("CRaC engine failed to provide its textual description");
    return;
  }
  tty->print_raw_cr(description);
  tty->cr();

  const crlib_conf_option_t *options = engine.configuration_options();
  if (options != nullptr) {
    if (pattern == nullptr) {
      tty->print_raw_cr("Configuration options:");
    } else {
      tty->print_cr("Configuration options matching *%s*:", pattern);
    }
    int matched = 0;
    for (; options->key != nullptr; ++options) {
      if (pattern == nullptr || strstr(options->key, pattern)) {
        tty->print_cr("* %s=<%s> (default: %s) - %s", options->key, options->value_type, options->default_value, options->description);
        ++matched;
      }
    }
    if (pattern != nullptr && matched == 0) {
      tty->print_raw_cr("(no configuration options match the pattern)");
    }
  } else {
    tty->print_raw_cr("Configuration options:");
    if (pattern != nullptr) {
      log_warning(crac)("Option filtering by pattern not available");
    }
    const char *conf_doc = engine.configuration_doc();
    if (conf_doc == nullptr) {
      log_error(crac)("CRaC engine failed to provide documentation of its configuration options");
      return;
    }
    tty->print_raw(conf_doc); // Doc string ends with CR by convention

    const GrowableArrayCHeap<const char *, MemTag::mtInternal> *controlled_opts = engine.vm_controlled_options();
    tty->cr();
    tty->print_raw("Configuration options controlled by the JVM: ");
    for (int i = 0; i < controlled_opts->length(); i++) {
      const char *opt = controlled_opts->at(i);
      tty->print_raw(opt);
      if (i < controlled_opts->length() - 1) {
        tty->print_raw(", ");
      }
    }
    tty->cr();
    delete controlled_opts;
  }

  vm_exit(0);
  ShouldNotReachHere();
}

template<class T> class FutureRef {
private:
  T *_t;
public:
  FutureRef(T *t): _t(t) {}
  ~FutureRef() {
    delete _t;
  }
  T *operator->() {
    return _t;
  }
  T *extract() {
    T *tmp = _t;
    _t = nullptr;
    return tmp;
  }
};

bool crac::prepare_checkpoint() {
  precond(CRaCCheckpointTo != nullptr);

  // Initialize CRaC engine now to verify all the related VM options
  assert(_engine == nullptr, "CRaC engine should be initialized only once");
  FutureRef<CracEngine> engine(new CracEngine());
  if (!engine->is_initialized()) {
    return false;
  }

  char image_location[PATH_MAX];
  bool fixed_path;
  if (!interpolate_checkpoint_location(image_location, PATH_MAX, &fixed_path)) {
    return false;
  }
  if (fixed_path && (!ensure_checkpoint_dir(image_location, true) || !engine->configure_image_location(image_location))) {
    return false;
  }

  _engine = engine.extract();
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
  log_debug(crac)("Checkpoint %i requested (dry run=%s)", os::current_process_id(), BOOL_TO_STR(dry_run));

  if (CRaCCheckpointTo == nullptr) {
    log_error(crac)("CRaCCheckpointTo is not specified");
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

#if INCLUDE_JVMTI
  JvmtiExport::post_crac_before_checkpoint();
#endif

  Universe::heap()->set_cleanup_unused(true);
  Universe::heap()->collect(GCCause::_full_gc_alot);
  Universe::heap()->set_cleanup_unused(false);

  if (os::can_trim_native_heap()) {
    os::size_change_t sc;
    if (os::trim_native_heap(&sc)) {
      if (sc.after != SIZE_MAX) {
        const size_t delta = sc.after < sc.before ? (sc.before - sc.after) : (sc.after - sc.before);
        const char sign = sc.after < sc.before ? '-' : '+';
        log_debug(crac)("Trim native heap before checkpoint: " PROPERFMT "->" PROPERFMT " (%c" PROPERFMT ")",
                        PROPERFMTARGS(sc.before), PROPERFMTARGS(sc.after), sign, PROPERFMTARGS(delta));
      }
    }
  }

  JFR_ONLY(Jfr::before_checkpoint();)

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

  Universe::heap()->after_restore();

  LogConfiguration::reopen();
  if (aio_writer) {
    aio_writer->resume();
  }

  JFR_ONLY(Jfr::after_restore();)

#if INCLUDE_JVMTI
  JvmtiExport::post_crac_after_restore();
#endif

  if (cr.ok()) {
    // Using handle rather than oop; dangling oop would fail with -XX:+CheckUnhandledOops
    Handle new_args;
    if (cr.new_args()) {
      oop args_oop = java_lang_String::create_oop_from_str(cr.new_args(), CHECK_NH);
      new_args = Handle(THREAD, args_oop);
    }

    GrowableArray<const char *>* new_properties = cr.new_properties();
    objArrayOop propsObj = oopFactory::new_objArray(vmClasses::String_klass(), new_properties->length(), CHECK_NH);
    objArrayHandle props(THREAD, propsObj);

    for (int i = 0; i < new_properties->length(); i++) {
      oop propObj = java_lang_String::create_oop_from_str(new_properties->at(i), CHECK_NH);
      props->obj_at_put(i, propObj);
    }

    wakeup_threads_in_timedwait();

    return ret_cr(JVM_CHECKPOINT_OK, new_args, props, Handle(), Handle(), THREAD);
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

bool crac::is_image_constraints_supported() {
  return _engine != nullptr && _engine->prepare_image_constraints_api() == CracEngine::ApiStatus::OK;
}

bool crac::record_image_label(const char *label, const char *value) {
  if (_engine == nullptr || _engine->prepare_image_constraints_api() != CracEngine::ApiStatus::OK) {
    return false;
  }
  return _engine->set_label(label, value);
}

bool crac::is_image_score_supported() {
  // The engine is not initialized when CRaCCheckpointTo is not set
  return _engine != nullptr && _engine->prepare_image_score_api() == CracEngine::ApiStatus::OK;
}

bool crac::record_image_score(jobjectArray metrics, jdoubleArray values) {
  if (_engine == nullptr || _engine->prepare_image_score_api() != CracEngine::ApiStatus::OK) {
    return false;
  }
  ResourceMark rm;
  objArrayOop metrics_oops = oop_cast<objArrayOop>(JNIHandles::resolve_non_null(metrics));
  typeArrayOop values_oops = oop_cast<typeArrayOop>(JNIHandles::resolve_non_null(values));
  assert(metrics_oops->length() == values_oops->length(), "should be equal");
  for (int i = 0; i < metrics_oops->length(); ++i) {
    oop metric_oop = metrics_oops->obj_at(i);
    assert(metric_oop != nullptr, "not null");
    const char* metric = java_lang_String::as_utf8_string(metric_oop);
    double value = values_oops->double_at(i);
    if (!_engine->set_score(metric, value)) {
      return false;
    }
  }

  bool result = true;
  result = result && _engine->set_score("java.lang.Runtime.availableProcessors", os::active_processor_count());
  result = result && _engine->set_score("java.lang.Runtime.totalMemory", Universe::heap()->capacity());
  result = result && _engine->set_score("java.lang.Runtime.maxMemory", Universe::heap()->max_capacity());

  double uptime = TimeHelper::counter_to_millis(os::elapsed_counter());
  result = result && _engine->set_score("vm.boot_time", os::javaTimeMillis() - uptime);
  result = result && _engine->set_score("vm.uptime", uptime);
  result = result && _engine->set_score("vm.uptime_since_restore", TimeHelper::counter_to_millis(os::elapsed_counter_since_restore()));

#if INCLUDE_MANAGEMENT
  jlong shared_loaded_classes = ClassLoadingService::loaded_shared_class_count();
  jlong shared_unloaded_classes = ClassLoadingService::unloaded_shared_class_count();
  // The keys match what jcmd <pid> PerfCounter.print would use
  result = result && _engine->set_score("java.cls.loadedClasses", ClassLoadingService::loaded_class_count() - shared_loaded_classes);
  result = result && _engine->set_score("java.cls.sharedLoadedClasses", shared_loaded_classes);
  result = result && _engine->set_score("java.cls.unloadedClasses", ClassLoadingService::unloaded_class_count() - shared_unloaded_classes);
  result = result && _engine->set_score("java.cls.sharedUnloadedClasses", shared_unloaded_classes);
#endif // INCLUDE_MANAGEMENT
  if (ClassLoader::perf_app_classload_count() != nullptr) {
    result = result && _engine->set_score("sun.cls.appClassLoadCount", ClassLoader::perf_app_classload_count()->get_value());
  }

  result = result && _engine->set_score("sun.ci.totalCompiles", CompileBroker::get_total_compile_count());
  result = result && _engine->set_score("sun.ci.totalBailouts", CompileBroker::get_total_bailout_count());
  result = result && _engine->set_score("sun.ci.totalInvalidates", CompileBroker::get_total_invalidated_count());
  // CompileBroker::get_total_native_compile_count() is never incremented?
  result = result && _engine->set_score("sun.ci.osrCompiles", CompileBroker::get_total_osr_compile_count());
  result = result && _engine->set_score("sun.ci.standardCompiles", CompileBroker::get_total_standard_compile_count());
  result = result && _engine->set_score("sun.ci.osrBytes", CompileBroker::get_sum_osr_bytes_compiled());
  result = result && _engine->set_score("sun.ci.standardBytes", CompileBroker::get_sum_standard_bytes_compiled());
  result = result && _engine->set_score("sun.ci.nmethodSize", CompileBroker::get_sum_nmethod_size());
  result = result && _engine->set_score("sun.ci.nmethodCodeSize", CompileBroker::get_sum_nmethod_code_size());
  result = result && _engine->set_score("java.ci.totalTime", CompileBroker::get_total_compilation_time());
  return result;
}

bool crac::record_image_score(const char *metric, double value) {
  if (_engine == nullptr || _engine->prepare_image_score_api() != CracEngine::ApiStatus::OK) {
    return false;
  }
  return _engine->set_score(metric, value);
}

void crac::prepare_restore(crac_restore_data& restore_data) {
  restore_data.restore_time = os::javaTimeMillis();
  restore_data.restore_nanos = os::javaTimeNanos();
}

void crac::restore(crac_restore_data& restore_data) {
  precond(CRaCRestoreFrom != nullptr);

  struct stat statbuf;
  if (os::stat(CRaCRestoreFrom, &statbuf) != 0) {
    log_error(crac)("Cannot open CRaCRestoreFrom=%s: %s", CRaCRestoreFrom, os::strerror(errno));
    return;
  }
  if ((statbuf.st_mode & S_IFMT) != S_IFDIR) {
    log_error(crac)("CRaCRestoreFrom=%s is not a directory", CRaCRestoreFrom);
    return;
  }

  // Note that this is a local, i.e. the handle will be destroyed if we fail to restore
  CracEngine engine;
  if (!engine.is_initialized() || !engine.configure_image_location(CRaCRestoreFrom)) {
    return;
  }

  // Previously IgnoreCPUFeatures didn't disable the check completely; the difference
  // was printed out but continued even despite features not being satisfied.
  // Since the check itself is delegated to the C/R Engine we will simply
  // skip the check here.
  bool ignore = VM_Version::ignore_cpu_features();
  bool exact = false;
  if (CheckCPUFeatures == nullptr || !strcmp(CheckCPUFeatures, "compatible")) {
    // default, compatible
  } else if (!strcmp(CheckCPUFeatures, "skip")) {
    ignore = true;
  } else if (!strcmp(CheckCPUFeatures, "exact")) {
    exact = true;
  } else {
    log_error(crac)("Invalid value for -XX:CheckCPUFeatures=%s; available are 'compatible', 'exact' or 'skip'", CheckCPUFeatures);
    return;
  }
  if (!ignore) {
    switch (engine.prepare_image_constraints_api()) {
      case CracEngine::ApiStatus::OK: {
        VM_Version::VM_Features current_features;
        if (VM_Version::cpu_features_binary(&current_features)) {
          engine.require_cpuinfo(&current_features, exact);
        }
        } break;
      case CracEngine::ApiStatus::ERR:
        return;
      case CracEngine::ApiStatus::UNSUPPORTED:
        log_warning(crac)("Cannot verify CPUFeatures for restore "
          "with the selected CRaC engine");
        break;
    }
  }

  switch (engine.prepare_restore_data_api()) {
    case CracEngine::ApiStatus::OK: {
      const int shmid = os::current_process_id();
      CracSHM shm(shmid);
      const int shmfd = shm.open(O_RDWR | O_CREAT | O_TRUNC);
      if (shmfd < 0) {
        log_error(crac)("Failed to open a space shared with restored process");
        return;
      }
      const bool write_success = CracRestoreParameters::write_to(
        shmfd,
        Arguments::jvm_restore_flags_array(), Arguments::num_jvm_restore_flags(),
        Arguments::system_properties(),
        !CRaCIgnoreRestoreIfUnavailable && Arguments::java_command_crac() != nullptr ?
          Arguments::java_command_crac() : "",
        restore_data.restore_time,
        restore_data.restore_nanos
      );
      close(shmfd);
      if (!write_success) {
        log_error(crac)("Failed to write to a space shared with restored process");
        return;
      }
      if (!engine.set_restore_data(&shmid, sizeof(shmid))) {
        log_error(crac)("CRaC engine failed to record restore data");
        return;
      }
      break;
    }
    case CracEngine::ApiStatus::ERR: break;
    case CracEngine::ApiStatus::UNSUPPORTED:
      log_warning(crac)("Cannot pass restore parameters (JVM flags, env vars, system properties, arguments...) "
        "with the selected CRaC engine");
      break;
  }

  const int ret = engine.restore();
  if (ret != 0) {
    log_error(crac)("CRaC engine failed to restore from %s: error %d", CRaCRestoreFrom, ret);
    VM_Version::VM_Features current_features;
    VM_Version::cpu_features_binary(&current_features); // ignore return value
    engine.check_cpuinfo(&current_features, exact);
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
        // A single ccstrlist flag can be specified multiple times meaning those
        // should be concatenated. But with the current code the last occurence
        // will just overwrite the previous ones.
        assert(!JVMFlag::find_flag(cursor)->ccstr_accumulates(),
               "setting ccstrlist flags on restore is not supported: %s", cursor);
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
  os::javaTimeSystemUTC(_checkpoint_wallclock_seconds, _checkpoint_wallclock_nanos);
  _checkpoint_monotonic_nanos = os::javaTimeNanos();
  memset(_checkpoint_bootid, 0, UUID_LENGTH);
  read_bootid(_checkpoint_bootid);
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
  if (!read_bootid(buf) || memcmp(buf, _checkpoint_bootid, UUID_LENGTH) != 0) {
    jlong current_wallclock_seconds;
    jlong current_wallclock_nanos;
    os::javaTimeSystemUTC(current_wallclock_seconds, current_wallclock_nanos);

    jlong diff_wallclock =
      (current_wallclock_seconds - _checkpoint_wallclock_seconds) * NANOSECS_PER_SEC +
      current_wallclock_nanos - _checkpoint_wallclock_nanos;
    // If the wall clock has gone backwards we won't add it to the offset
    if (diff_wallclock < 0) {
      diff_wallclock = 0;
    }

    // javaTimeNanos() call on the second line below uses the *_offset, so we will zero
    // it to make the call return true monotonic time rather than the adjusted value.
    _javaTimeNanos_offset = 0;
    _javaTimeNanos_offset = _checkpoint_monotonic_nanos - os::javaTimeNanos() + diff_wallclock;
  } else {
    // ensure monotonicity even if this looks like the same boot
    jlong diff = os::javaTimeNanos() - _checkpoint_monotonic_nanos;
    if (diff < 0) {
      _javaTimeNanos_offset -= diff;
    }
  }
}
