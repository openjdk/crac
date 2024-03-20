#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "classfile/vmSymbols.hpp"
#include "interpreter/bytecodes.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/cracStackDumper.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackValue.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframe_hp.hpp"
#include "utilities/basicTypeWriter.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/methodKind.hpp"

static uintptr_t oop2uint(oop o) {
  STATIC_ASSERT(sizeof(uintptr_t) == sizeof(intptr_t)); // Primitive stack slots
  STATIC_ASSERT(sizeof(uintptr_t) == oopSize);          // IDs
  return cast_from_oop<uintptr_t>(o);
}

// Retrieves Java vframes from all non-internal Java threads in the VM.
class ThreadStackStream : public StackObj {
 public:
  enum class Status { OK, END, NON_JAVA_IN_MID, NON_JAVA_ON_TOP };

  Status next() {
    if (!_started) {
      _started = true;
    } else {
      _thread_i++;
    }

    JavaThread *thread;
    for (; _thread_i < _tlh.length(); _thread_i++) {
      thread = _tlh.thread_at(_thread_i);
      assert(thread->thread_state() == _thread_in_native || thread->thread_state() == _thread_blocked,
             "must be on safepoint: either blocked or in native code");
      if (should_include(*thread)) {
        break;
      }
      if (log_is_enabled(Debug, crac, stacktrace, dump)) {
        ResourceMark rm;
        log_debug(crac, stacktrace, dump)("Skipping thread " UINTX_FORMAT " (%s)", oop2uint(thread->threadObj()), thread->name());
      }
    }
    if (_thread_i == _tlh.length()) {
      return Status::END;
    }
    postcond(thread != nullptr);

    if (log_is_enabled(Debug, crac, stacktrace, dump)) {
      ResourceMark rm;
      log_debug(crac, stacktrace, dump)("Dumping thread " UINTX_FORMAT " (%s): state = %s",
                                        oop2uint(thread->threadObj()), thread->name(), thread->thread_state_name());
    }

    _frames.clear();
    vframeStream vfs(thread, /*stop_at_java_call_stub=*/ true);

    if (!vfs.at_end() && vfs.method()->is_native() && !is_special_native_method(*vfs.method())) {
      if (log_is_enabled(Debug, crac, stacktrace, dump)) {
        ResourceMark rm;
        log_debug(crac, stacktrace, dump)("Thread " UINTX_FORMAT " (%s) is executing native method %s",
                                          oop2uint(thread->threadObj()), thread->name(), vfs.method()->external_name());
      }
      return Status::NON_JAVA_ON_TOP;
    }

    DEBUG_ONLY(bool is_youngest_frame = true);
    for (; !vfs.at_end(); vfs.next()) {
      assert(is_youngest_frame || !vfs.method()->is_native(), "only the youngest frame can be native");
      DEBUG_ONLY(is_youngest_frame = false);
      if (vfs.method()->is_old()) {
        ResourceMark rm;
        log_warning(crac, stacktrace, dump)("JVM TI support will be required on restore: thread %s executes an old version of %s",
                                            thread->name(), vfs.method()->external_name());
        Unimplemented(); // TODO extend dump format with method holder's redefinition version
      }
      _frames.push(vfs.asJavaVFrame());
    }

    if (_frames.is_empty() || vfs.reached_first_entry_frame()) {
      return Status::OK;
    }

    if (log_is_enabled(Debug, crac, stacktrace, dump)) {
      ResourceMark rm;
      log_debug(crac, stacktrace, dump)("Thread " UINTX_FORMAT " (%s) has intermediate non-Java frame after %i Java frames",
                                        oop2uint(thread->threadObj()), thread->name(), _frames.length());
    }
    return Status::NON_JAVA_IN_MID;
  }

  JavaThread *thread()                            const { assert(_started, "call next() first"); return _tlh.thread_at(_thread_i); }
  const GrowableArrayView<javaVFrame *> &frames() const { assert(_started, "call next() first"); return _frames; };

 private:
  bool _started = false;
  ThreadsListHandle _tlh;
  uint _thread_i = 0;
  GrowableArray<javaVFrame *> _frames;

  // Whether this thread should be included in the dump.
  static bool should_include(const JavaThread &thread) {
    if (thread.is_exiting() ||
        thread.is_hidden_from_external_view() ||
        thread.is_Compiler_thread() ||
        thread.is_Notification_thread() ||
        thread.is_AttachListener_thread() || // TODO jcmd support will probably require this to be treated specially
        thread.is_jvmti_agent_thread()) {    // TODO JVM TI support: these are user-provided, need to think it through
      return false;
    }
    // TODO
    // 1. This way of identification is not fully accurate: the user can also
    //    create threads that would match.
    // 2. All threads identified below, except Signal Dispatcher, are created
    //    from Java, so we'll include them too when restoration of system
    //    classes is supported.
    const oop tg = java_lang_Thread::threadGroup(thread.threadObj());
    if (tg == Universe::system_thread_group()) {
      ResourceMark rm;
      const char *thread_name = thread.name();
      if (strcmp(thread_name, "Signal Dispatcher") == 0 ||
          strcmp(thread_name, "Finalizer") == 0 ||
          strcmp(thread_name, "Reference Handler") == 0) {
        return false;
      }
    } else {
      ResourceMark rm;
      if (strcmp(thread.name(), "Common-Cleaner") == 0 &&
          strcmp(java_lang_ThreadGroup::name(tg), "InnocuousThreadGroup") == 0) {
        return false;
      }
    }
    return true;
  }

  // Whether this is a native method known how to restore.
  static bool is_special_native_method(const Method &m) {
    precond(m.is_native());
    const InstanceKlass &holder = *m.method_holder();
    return // CRaC's C/R method
           (holder.name() == vmSymbols::jdk_crac_Core() &&
            holder.class_loader_data()->is_the_null_class_loader_data() &&
            m.name() == vmSymbols::checkpointRestore0_name()) ||
           // Unsafe.park(...)
           m.intrinsic_id() == vmIntrinsics::_park;
  }
};

class StackDumpWriter : public StackObj {
 private:
  BasicTypeWriter *_writer;

#define WRITE(value) do { if (!_writer->write(value))                     return false; } while (false)
#define WRITE_RAW(value, size) do { if (!_writer->write_raw(value, size)) return false; } while (false)
#define WRITE_CASTED(t, value) do { if (!_writer->write<t>(value))        return false; } while (false)

 public:
  explicit StackDumpWriter(BasicTypeWriter *writer) : _writer(writer) {}

  bool write_header() {
    constexpr char HEADER[] = "CRAC STACK DUMP 0.1";
    WRITE_RAW(HEADER, sizeof(HEADER));

    STATIC_ASSERT(sizeof(uintptr_t) == sizeof(u4) || sizeof(uintptr_t) == sizeof(u8));
    WRITE_CASTED(u2, sizeof(uintptr_t)); // Word size

    return true;
  }

  bool write_stack(const JavaThread *thread, const GrowableArrayView<javaVFrame *> &frames) {
    log_trace(crac, stacktrace, dump)("Stack for thread " UINTX_FORMAT " (%s)", oop2uint(thread->threadObj()), thread->name());
    WRITE(oop2uint(thread->threadObj())); // Thread ID

    log_trace(crac, stacktrace, dump)("%i frames", frames.length());
    WRITE_CASTED(u4, frames.length()); // Number of frames in the stack

    for (int i = 0; i < frames.length(); i++) {
      const javaVFrame &frame = *frames.at(i);
      if (log_is_enabled(Trace, crac, stacktrace, dump)) {
        if (frame.is_interpreted_frame()) {
          log_trace(crac, stacktrace, dump)("== Interpreted frame ==");
        } else {
          precond(frame.is_compiled_frame());
          log_trace(crac, stacktrace, dump)("==  Compiled frame   ==");
          // TODO use Deoptimization::realloc_objects(...) to rematerialize
          //  scalar-replaced objects
        }
      }

      if (!write_method(frame)) {
        return false;
      }

      u2 bci = checked_cast<u2>(frame.bci()); // u2 is enough -- guaranteed by JVMS §4.7.3 (code_length max value)
      // If this is the youngest frame and the current bytecode has already been
      // executed move to the next one
      // TODO investigate whether:
      //  1. For interpreted frame, is it always right to re-execute?
      //  2. For compiled frame, is exec_mode used by deoptimization to decide
      //     on re-execution also important for us here?
      if (i == 0 && !frame.method()->is_native() &&
          frame.is_compiled_frame() && !static_cast<const compiledVFrame &>(frame).should_reexecute()) {
        const int code_len = Bytecodes::length_at(frame.method(), frame.method()->bcp_from(frame.bci()));
        log_trace(crac, stacktrace, dump)("moving BCI: %i -> %i", bci, bci + code_len);
        assert(bci + code_len <= UINT16_MAX, "overflow");
        bci += code_len;
      }
      guarantee(frame.method()->validate_bci(bci) >= 0, "invalid BCI %i for %s", bci, frame.method()->external_name());
      if (log_is_enabled(Trace, crac, stacktrace, dump)) {
        const char *code_name;
        if (!frame.method()->is_native()) {
          code_name = Bytecodes::name(frame.method()->java_code_at(bci));
        } else {
          assert(bci == 0, "no bytecodes in a native method");
          code_name = "native entrance";
        }
        log_trace(crac, stacktrace, dump)("BCI: %i (%s)", bci, code_name);
      }
      WRITE(bci);

      log_trace(crac, stacktrace, dump)("Locals:");
      if (!write_stack_values(*frame.locals())) {
        return false;
      }

      log_trace(crac, stacktrace, dump)("Operands:");
      if (!write_stack_values(*frame.expressions())) {
        return false;
      }

      log_trace(crac, stacktrace, dump)("Monitors: not implemented");
      WRITE_CASTED(u2, 0);

      log_trace(crac, stacktrace, dump)("=======================");
    }

    return true;
  }

 private:
  bool write_method(const javaVFrame &frame) {
    Method *method = frame.method();

    Symbol *name = method->name();
    if (log_is_enabled(Trace, crac, stacktrace, dump)) {
      ResourceMark rm;
      log_trace(crac, stacktrace, dump)("Method name: " UINTX_FORMAT " - %s", reinterpret_cast<uintptr_t>(name), name->as_C_string());
    }
    WRITE(reinterpret_cast<uintptr_t>(name));

    Symbol *signature = method->signature();
    if (log_is_enabled(Trace, crac, stacktrace, dump)) {
      ResourceMark rm;
      log_trace(crac, stacktrace, dump)("Method signature: " UINTX_FORMAT " - %s", reinterpret_cast<uintptr_t>(signature), signature->as_C_string());
    }
    WRITE(reinterpret_cast<uintptr_t>(signature));

    const MethodKind::Enum kind = MethodKind::of_method(*method);
    log_trace(crac, stacktrace, dump)("Method kind: %s", MethodKind::name(kind));
    WRITE(checked_cast<u1>(kind));

    InstanceKlass *holder = method->method_holder();
    if (log_is_enabled(Trace, crac, stacktrace, dump)) {
      ResourceMark rm;
      log_trace(crac, stacktrace, dump)("Class: " UINTX_FORMAT " - %s", oop2uint(holder->java_mirror()), holder->external_name());
    }
    WRITE(oop2uint(holder->java_mirror()));

    return true;
  }

  bool write_stack_values(const StackValueCollection &values) {
    assert(values.size() <= UINT16_MAX, "guaranteed by JVMS §4.11");
    log_trace(crac, stacktrace, dump)("%i values", values.size());
    WRITE_CASTED(u2, values.size());

    for (int i = 0; i < values.size(); i++) {
      const StackValue &value = *values.at(i);
      switch (value.type()) {
        case T_INT:
          log_trace(crac, stacktrace, dump)("  %i - primitive: " INTX_FORMAT " (intptr), " INT32_FORMAT " (jint), " UINTX_FORMAT_X " (hex)",
                                            i, value.get_intptr(), value.get_jint(), value.get_intptr());
          WRITE_CASTED(u1, DumpedStackValueType::PRIMITIVE);
          WRITE_CASTED(uintptr_t, value.get_intptr()); // Write the whole slot, i.e. 4 or 8 bytes
          break;
        case T_OBJECT:
          log_trace(crac, stacktrace, dump)("  %i - oop: " UINTX_FORMAT "%s",
                                            i, oop2uint(value.get_obj()()), value.obj_is_scalar_replaced() ? " (scalar-replaced)" : "");
          guarantee(!value.obj_is_scalar_replaced(), "Scalar-replaced objects should have been rematerialized");
          WRITE_CASTED(u1, DumpedStackValueType::REFERENCE);
          WRITE(oop2uint(value.get_obj()()));
          break;
        case T_CONFLICT: // Compiled frames may contain these
          log_trace(crac, stacktrace, dump)("  %i - dead (dumping as 0)", i);
          WRITE_CASTED(u1, DumpedStackValueType::PRIMITIVE);
          // Deopt code says this should be zero/null in case it is actually
          // a reference to prevent GC from following it
          WRITE_CASTED(uintptr_t, 0);
          break;
        default:
          ShouldNotReachHere();
      }
    }

    return true;
  }

#undef WRITE_CASTED
#undef WRITE_RAW
#undef WRITE
};

CracStackDumper::Result CracStackDumper::dump(const char *path, bool overwrite) {
  assert(SafepointSynchronize::is_at_safepoint(),
         "need safepoint so threads won't change their states after we check them");
  log_info(crac, stacktrace, dump)("Dumping thread stacks into %s", path);

  FileBasicTypeWriter file_writer;
  if (!file_writer.open(path, overwrite)) {
    return {Result::Code::IO_ERROR, os::strerror(errno)};
  }

  StackDumpWriter dump_writer(&file_writer);
  if (!dump_writer.write_header()) {
    return {Result::Code::IO_ERROR, "failed to write into the opened file"};
  }

  ResourceMark rm; // Frames are resource-allocated
  ThreadStackStream tss;
  ThreadStackStream::Status tss_status = tss.next();
  for (; tss_status == ThreadStackStream::Status::OK; tss_status = tss.next()) {
    if (!dump_writer.write_stack(tss.thread(), tss.frames())) {
      return {Result::Code::IO_ERROR, "failed to write into the opened file"};
    }
  }
  switch (tss_status) {
    case ThreadStackStream::Status::OK:              ShouldNotReachHere();
    case ThreadStackStream::Status::END:             return {};
    case ThreadStackStream::Status::NON_JAVA_ON_TOP: return {Result::Code::NON_JAVA_ON_TOP, tss.thread()};
    case ThreadStackStream::Status::NON_JAVA_IN_MID: return {Result::Code::NON_JAVA_IN_MID, tss.thread()};
  }

  // Make the compiler happy
  ShouldNotReachHere();
  return {};
}
