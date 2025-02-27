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
#include "crlib/crlib.h"
#include "crlib/crlib_restore_data.h"
#include "jfr/jfr.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/allocation.hpp"
#include "memory/oopFactory.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/crac_structs.hpp"
#include "runtime/crac.hpp"
#include "runtime/globals.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "services/writeableFlags.hpp"
#include "utilities/decoder.hpp"
#include "os.inline.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/globalDefinitions.hpp"

static jlong _restore_start_time;
static jlong _restore_start_nanos;

const crac::EngineHandle *crac::_engine = nullptr;
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

#ifdef _WINDOWS
static char *strsep(char **strp, const char *delim) {
  char *str = *strp;
  if (str == nullptr) {
    return nullptr;
  }
  size_t len = strcspn(str, delim);
  if (str[len] == '\0') {
    *strp = nullptr;
    return str;
  }
  str[len] = '\0';
  *strp += len + 1;
  return str;
}
#endif // _WINDOWS

class crac::EngineHandle : public CHeapObj<mtInternal> {
private:
  void *_lib = nullptr;
  crlib_api_t *_api = nullptr;
  crlib_conf_t *_conf = nullptr;

public:
  // Use is_initialized() to check whether the constructor succeeded.
  explicit EngineHandle(bool checkpoint);
  ~EngineHandle();

  crlib_api_t *api() const   { return _api; }
  crlib_conf_t *conf() const { return _conf; }

  bool is_initialized() const {
    assert((_lib == nullptr && _api == nullptr && _conf == nullptr) ||
           (_lib != nullptr && _api != nullptr && _conf != nullptr), "invariant");
    return _lib != nullptr;
  }
};

static bool find_crac_engine(const char *dll_dir, char *path, size_t path_size, bool *is_library) {
  // Try to interpret as a file path
  if (os::is_path_absolute(CRaCEngine)) {
    const size_t path_len = strlen(CRaCEngine);
    if (path_len + 1 > path_size) {
      log_error(crac)("CRaCEngine file path is too long: %s", CRaCEngine);
      return false;
    }

    if (!os::file_exists(CRaCEngine)) {
      log_error(crac)("CRaCEngine file does not exist: %s", CRaCEngine);
      return false;
    }

    strcpy(path, CRaCEngine);

    const char *last_slash = strrchr(CRaCEngine, *os::file_separator());
    const char *basename;
    if (last_slash == nullptr) {
      basename = CRaCEngine;
    } else {
      basename = last_slash + strlen(os::file_separator());
    }
    *is_library = strncmp(basename, JNI_LIB_PREFIX, strlen(JNI_LIB_PREFIX)) == 0 &&
      strcmp(path + path_len - strlen(JNI_LIB_SUFFIX), JNI_LIB_SUFFIX) == 0;
    log_debug(crac)("CRaCEngine path %s is %s library", CRaCEngine, *is_library ? "a" : "not a");

    return true;
  }

  // Try to interpret as a library name
  if (os::dll_locate_lib(path, path_size, dll_dir, CRaCEngine)) {
    *is_library = true;
    log_debug(crac)("Found CRaCEngine %s as a library in %s", CRaCEngine, path);
    return true;
  }

  *is_library = false;
  log_debug(crac)("CRaCEngine %s is not a library in %s", CRaCEngine, dll_dir);

#ifdef _WINDOWS
  const char *suffix = ".exe";
#else
  const char *suffix = "";
#endif // ! _WINDOWS
#ifndef S_ISREG
# define S_ISREG(__mode) ((__mode & S_IFMT) == S_IFREG)
#endif // S_ISREG
  struct stat st;

  // Try to interpret as an executable name with "engine" suffix omitted
  size_t path_len = strlen(dll_dir) + strlen(os::file_separator()) + strlen(CRaCEngine) + strlen("engine") + strlen(suffix);
  if (path_len + 1 <= path_size) {
    os::snprintf_checked(path, path_size, "%s%s%sengine%s", dll_dir, os::file_separator(), CRaCEngine, suffix);
    if (os::stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      log_debug(crac)("Found CRaCEngine %s as %s", CRaCEngine, path);
      return true;
    }
  } else {
    log_debug(crac)("Not looking for CRaCEngine an executable name with 'engine' omitted: path is too long");
  }

  // Try to interpret as an executable name
  precond(path_len > strlen("engine"));
  path_len -= strlen("engine");
  if (path_len + 1 <= path_size) {
    os::snprintf_checked(path, path_size, "%s%s%s%s", dll_dir, os::file_separator(), CRaCEngine, suffix);
    if (os::stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      log_debug(crac)("Found CRaCEngine %s as %s", CRaCEngine, path);
      return true;
    }
  } else {
    log_debug(crac)("Not looking for CRaCEngine as an executable name: path is too long");
  }

  return false;
}

// CRaC engine configuration options JVM sets directly instead of relaying from the user
#define ENGINE_OPT_IMAGE_LOCATION "image_location"
#define ENGINE_OPT_EXEC_LOCATION "exec_location"

static bool configure_image_location(const crlib_api_t &api, crlib_conf_t *conf, const char *image_location) {
  precond(image_location != nullptr && image_location[0] != '\0');
  if (!api.configure(conf, ENGINE_OPT_IMAGE_LOCATION, image_location)) {
    log_error(crac)("CRaC engine failed to configure: '" ENGINE_OPT_IMAGE_LOCATION "' = '%s'", image_location);
    return false;
  }
  return true;
}

static crlib_conf_t *create_engine_conf(
    const crlib_api_t &api, const char *image_location, const char *exec_location) {
  crlib_conf_t * const conf = api.create_conf();
  if (conf == nullptr) {
    log_error(crac)("CRaC engine failed to create its configuration");
    return nullptr;
  }

  if (!configure_image_location(api, conf, image_location)) {
    api.destroy_conf(conf);
    return nullptr;
  }

  if (exec_location != nullptr) { // Only passed when using crexec
    guarantee(api.can_configure(conf, ENGINE_OPT_EXEC_LOCATION),
              "crexec does not support an internal option: " ENGINE_OPT_EXEC_LOCATION);
    if (!api.configure(conf, ENGINE_OPT_EXEC_LOCATION, exec_location)) {
      log_error(crac)("crexec failed to configure: '" ENGINE_OPT_EXEC_LOCATION "' = '%s'", exec_location);
      api.destroy_conf(conf);
      return nullptr;
    }
  }

  if (CRaCEngineOptions == nullptr || CRaCEngineOptions[0] == '\0' /* possible for ccstrlist */) {
    return conf;
  }

  char *engine_options = os::strdup_check_oom(CRaCEngineOptions, mtInternal);
  char *const engine_options_start = engine_options;
  do {
    char *key_value = strsep(&engine_options, ",");
    const char *key = strsep(&key_value, "=");
    const char *value = key_value != nullptr ? key_value : "";
    assert(key != nullptr, "Should have terminated before");
    if (strcmp(key, ENGINE_OPT_IMAGE_LOCATION) == 0 ||
        (exec_location != nullptr && strcmp(key, ENGINE_OPT_EXEC_LOCATION) == 0)) {
      log_warning(crac)("Internal CRaC engine option provided, skipping: %s", key);
      continue;
    }
    if (!api.configure(conf, key, value)) {
      log_error(crac)("CRaC engine failed to configure: '%s' = '%s'", key, value);
      os::free(engine_options_start);
      api.destroy_conf(conf);
      return nullptr;
    }
    log_debug(crac)("CRaC engine option: '%s' = '%s'", key, value);
  } while (engine_options != nullptr);
  os::free(engine_options_start);

  return conf;
}

crac::EngineHandle::EngineHandle(bool checkpoint) {
  if (CRaCEngine == nullptr) {
    log_error(crac)("CRaCEngine must not be empty");
    return;
  }

  // Arguments::get_dll_dir() might not have been initialized yet
  char dll_dir[JVM_MAXPATHLEN];
  os::jvm_path(dll_dir, sizeof(dll_dir));
  // path is ".../lib/server/libjvm.so", or "...\bin\server\libjvm.dll"
  char *after_elem = nullptr;
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(dll_dir, *os::file_separator());
    *after_elem = '\0';
  }

  char path[JVM_MAXPATHLEN];
  bool is_library;
  if (!find_crac_engine(dll_dir, path, sizeof(path), &is_library)) {
    log_error(crac)("Cannot find CRaC engine %s", CRaCEngine);
    return;
  }
  postcond(path[0] != '\0');

  char exec_path[JVM_MAXPATHLEN] = "\0";
  if (!is_library) {
    strcpy(exec_path, path); // Save to later pass it to crexec
    if (!os::dll_locate_lib(path, sizeof(path), dll_dir, "crexec")) {
      log_error(crac)("Cannot find crexec library to use CRaCEngine executable");
      return;
    }
  }

  char error_buf[1024];
  void * const lib = os::dll_load(path, error_buf, sizeof(error_buf));
  if (lib == nullptr) {
    log_error(crac)("Cannot load CRaC engine library from %s: %s", path, error_buf);
    return;
  }

  using api_func_t = decltype(&CRLIB_API);
  const auto api_func = reinterpret_cast<api_func_t>(os::dll_lookup(lib, CRLIB_API_FUNC));
  if (api_func == nullptr) {
    log_error(crac)("Cannot load CRaC engine library entrypoint '" CRLIB_API_FUNC "' from %s", path);
    os::dll_unload(lib);
    return;
  }

  crlib_api_t * const api = api_func(CRLIB_API_VERSION, sizeof(crlib_api_t));
  if (api == nullptr) {
    log_error(crac)("CRaC engine failed to initialize its API (version %i). "
                    "Maybe this version is not supported?", CRLIB_API_VERSION);
    os::dll_unload(lib);
    return;
  }
  if (api->create_conf == nullptr || api->destroy_conf == nullptr ||
      api->checkpoint == nullptr || api->restore == nullptr ||
      api->can_configure == nullptr || api->configure == nullptr ||
      api->get_extension == nullptr) {
    log_error(crac)("CRaC engine failed to fully initialize its API");
    os::dll_unload(lib);
    return;
  }

  const char *image_location = checkpoint ? CRaCCheckpointTo : CRaCRestoreFrom;
  const char *exec_location = exec_path[0] != '\0' ? exec_path : nullptr;
  crlib_conf_t * const conf = create_engine_conf(*api, image_location, exec_location);
  if (conf == nullptr) {
    os::dll_unload(lib);
    return;
  }

  _lib = lib;
  _api = api;
  _conf = conf;
}

crac::EngineHandle::~EngineHandle() {
  if (is_initialized()) {
    _api->destroy_conf(_conf);
    os::dll_unload(_lib);
  }
}

static crlib_restore_data_t *get_restore_data_api(const crlib_api_t *api) {
  precond(api != nullptr);
  crlib_restore_data_t * const restore_data_api = CRLIB_EXTENSION_RESTORE_DATA(api);
  if (restore_data_api == nullptr) {
    log_debug(crac)("CRaC engine does not support extension: " CRLIB_EXTENSION_RESTORE_DATA_NAME);
    return nullptr;
  }
  if (restore_data_api->set_restore_data == nullptr || restore_data_api->get_restore_data == nullptr) {
    log_debug(crac)("CRaC engine failed to fully initialize API extension: %s", CRLIB_EXTENSION_RESTORE_DATA_NAME);
    return nullptr;
  }
  return restore_data_api;
}

int crac::checkpoint_restore(int *shmid) {
  guarantee(_engine != nullptr, "CRaC engine is not initialized");

  crac::record_time_before_checkpoint();

  // CRaCCheckpointTo can be changed on restore so we need to update the conf
  // to account for that.
  // Note that CRaCEngine and CRaCEngineOptions are not updated (as documented)
  // so we don't need to re-init the whole engine handle.
  // TODO: do this only if there has been at least one restore (cannot check via
  //  CRaCRestoreFrom != nullptr because it can remain unset even after restore)
  if (!configure_image_location(*_engine->api(), _engine->conf(), CRaCCheckpointTo)) {
    return JVM_CHECKPOINT_ERROR;
  }

  const int ret = _engine->api()->checkpoint(_engine->conf());
  if (ret != 0) {
    log_error(crac)("CRaC engine failed to checkpoint to %s: error %i", CRaCCheckpointTo, ret);
    return JVM_CHECKPOINT_ERROR;
  }

  const auto *restore_data_api = get_restore_data_api(_engine->api());
  if (restore_data_api == nullptr) {
    *shmid = 0; // Not an error, just no restore data
  } else if (restore_data_api->get_restore_data(_engine->conf(), shmid, sizeof(*shmid)) < sizeof(*shmid)) {
    log_error(crac)("CRaC engine failed to provide restore data");
    *shmid = -1; // Error
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

  if (!ok && CRaCDoThrowCheckpointException) {
    return;
  } else if (_dry_run) {
    _ok = ok;
    return;
  }

  if (!memory_checkpoint()) {
    return;
  }

  int shmid = -1;
  if (CRaCAllowToSkipCheckpoint) {
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

  // It needs to check CPU features before any other code (such as VM_Crac::read_shm) depends on them.
  VM_Version::crac_restore();
  Arguments::reset_for_crac_restore();

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

  // VM_Crac::read_shm needs to be already called to read RESTORE_SETTABLE parameters.
  VM_Version::crac_restore_finalize();

  memory_restore();

  wakeup_threads_in_timedwait_vm();

  _ok = true;
}

bool crac::prepare_checkpoint() {
  precond(CRaCCheckpointTo != nullptr);

  struct stat st;
  if (0 == os::stat(CRaCCheckpointTo, &st)) {
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
      log_error(crac)("CRaCCheckpointTo=%s is not a directory", CRaCCheckpointTo);
      return false;
    }
  } else {
    if (-1 == os::mkdir(CRaCCheckpointTo)) {
      log_error(crac)("Cannot create CRaCCheckpointTo=%s: %s", CRaCCheckpointTo, os::strerror(errno));
      return false;
    }
    if (-1 == os::rmdir(CRaCCheckpointTo)) {
      log_warning(crac)("Cannot cleanup after CRaCCheckpointTo check: %s", os::strerror(errno));
      // not fatal
    }
  }

  // Initialize CRaC engine now to verify all the related VM options
  assert(_engine == nullptr, "CRaC engine should be initialized only once");
  _engine = new EngineHandle(true);
  if (!_engine->is_initialized()) {
    delete _engine;
    _engine = nullptr;
  }
  return _engine != nullptr;
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

  if (-1 == os::mkdir(CRaCCheckpointTo) && errno != EEXIST) {
    log_error(crac)("Cannot create CRaCCheckpointTo=%s: %s", CRaCCheckpointTo, os::strerror(errno));
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

#if INCLUDE_JVMTI
  JvmtiExport::post_crac_before_checkpoint();
#endif

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
  const EngineHandle engine = EngineHandle(false);
  if (!engine.is_initialized()) {
    return;
  }

  const auto *restore_data_api = get_restore_data_api(engine.api());
  if (restore_data_api != nullptr) {
    const int shmid = os::current_process_id();
    CracSHM shm(shmid);
    const int shmfd = shm.open(O_RDWR | O_CREAT | O_TRUNC);
    if (shmfd < 0) {
      log_error(crac)("Failed to open a space shared with restored process");
      return;
    }
    const bool write_success = CracRestoreParameters::write_to(
      shmfd,
      Arguments::jvm_flags_array(), Arguments::num_jvm_flags(),
      Arguments::system_properties(),
      Arguments::java_command_crac() ? Arguments::java_command_crac() : "",
      restore_data.restore_time,
      restore_data.restore_nanos
    );
    close(shmfd);
    if (!write_success) {
      log_error(crac)("Failed to write to a space shared with restored process");
      return;
    }
    if (!restore_data_api->set_restore_data(engine.conf(), &shmid, sizeof(shmid))) {
      log_error(crac)("CRaC engine failed to record restore data");
      return;
    }
  } else {
    log_warning(crac)("Cannot pass restore parameters (JVM flags, env vars, system properties, arguments...) "
                      "with the selected CRaC engine");
  }

  const int ret = engine.api()->restore(engine.conf());
  if (ret != 0) {
    log_error(crac)("CRaC engine failed to restore from %s: error %i", CRaCRestoreFrom, ret);
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
    } else if (strncmp(name, "CRaCEngine", ARRAY_SIZE("CRaCEngine") - 1) == 0) {
      // CRaCEngine and CRaCEngineOptions are not updated from the restoring process
      assert(strncmp(name, "CRaCEngine=", strlen("CRaCEngine=")) == 0 ||
             strncmp(name, "CRaCEngineOptions=", strlen("CRaCEngineOptions=")) == 0,
             "unexpected CRaCEngine* flag: %s", name);
      result = JVMFlag::Error::SUCCESS;
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
