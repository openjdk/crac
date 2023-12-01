#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackValue.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframe_hp.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/stackDumper.hpp"
#include <type_traits>

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

    for (; _thread_i < _tlh.length(); _thread_i++) {
      JavaThread *thread = _tlh.thread_at(_thread_i);
      if (!should_include(thread)) {
        if (log_is_enabled(Debug, stackdump)) {
          ResourceMark rm;
          log_debug(stackdump)("Skipping thread %p (%s)", thread, thread->name());
        }
        continue;
      }
      if (thread->thread_state() < _thread_in_Java) {
        if (log_is_enabled(Debug, stackdump)) {
          ResourceMark rm;
          log_debug(stackdump)("Thread %p (%s) not in Java: state = %i",
                               thread, thread->name(), thread->thread_state());
        }
        return Status::NON_JAVA_ON_TOP;
      }
      if (log_is_enabled(Debug, stackdump)) {
        ResourceMark rm;
        log_debug(stackdump)("Will try to dump thread %p (%s): state = %i",
                             thread, thread->name(), thread->thread_state());
      }

      _frames.clear();
      vframeStream vfs(thread, /* stop_at_java_call_stub = */ true);
      for (; !vfs.at_end(); vfs.next()) {
        if (!vfs.method()->is_native()) {
          _frames.push(vfs.asJavaVFrame());
        } else {
          guarantee(_frames.is_empty(), "Native frame must be the youngest in the series of Java frames");
          if (log_is_enabled(Debug, stackdump)) {
            ResourceMark rm;
            log_debug(stackdump)("Thread %p (%s) not in Java: its current method %s is native",
                                 thread, thread->name(), vfs.method()->external_name());
          }
          return Status::NON_JAVA_ON_TOP;
        }
      }

      if (_frames.is_empty() || vfs.reached_first_entry_frame()) {
        return Status::OK;
      }

      if (log_is_enabled(Debug, stackdump)) {
        ResourceMark rm;
        log_debug(stackdump)("Thread %p (%s) has intermediate non-Java frame after %i Java frames",
                             thread, thread->name(), _frames.length());
      }
      return Status::NON_JAVA_IN_MID;
    }

    postcond(_thread_i == _tlh.length());
    return Status::END;
  }

  JavaThread *thread()                            const { assert(_started, "Call next() first"); return _tlh.thread_at(_thread_i); }
  const GrowableArrayView<javaVFrame *> &frames() const { assert(_started, "Call next() first"); return _frames; };

 private:
  bool _started = false;
  ThreadsListHandle _tlh;
  uint _thread_i = 0;
  GrowableArray<javaVFrame *> _frames;

  static bool should_include(JavaThread *thread) {
    ResourceMark rm; // Thread name
    // TODO for now we only include the main thread, but there seems to be no
    //  way to reliably determine that a thread is the main thread.
    //
    // Not excluding JVMTI agent and AttachListener threads since they may
    // execute user-visible operations
    // if (thread->is_exiting() ||
    //     thread->is_hidden_from_external_view() ||
    //     thread->is_Compiler_thread() ||
    //     (strcmp(thread->name(), "Notification Thread") == 0 && java_lang_Thread::threadGroup(thread->threadObj()) == Universe::system_thread_group())) {
    //   continue;
    // }
    return !thread->is_exiting() &&
           java_lang_Thread::threadGroup(thread->threadObj()) == Universe::main_thread_group() && strcmp(thread->name(), "main") == 0;
  }
};

// Opens a file and writes into it.
//
// Class name "FileWriter" is already occupied by the heap dumping code.
class BinaryFileWriter : public StackObj {
 public:
  ~BinaryFileWriter() {
    if (_file != nullptr && fclose(_file) != 0) {
      log_error(stackdump)("Failed to close a stack dump file");
    }
  }

  bool open(const char *path, bool overwrite) {
    assert(path != nullptr, "Cannot write to null path");

    errno = 0;

    if (_file != nullptr && fclose(_file) != 0) {
      log_error(stackdump)("Failed to close a stack dump file");
      guarantee(errno != 0, "fclose should set errno on error");
      return false;
    }
    guarantee(errno == 0, "fclose shouldn't set errno on success");

    if (!overwrite && os::file_exists(path)) {
      errno = EEXIST;
      return false;
    }
    errno = 0; // os::file_exists() might have changed it

    FILE *file = os::fopen(path, "wb");
    if (file == nullptr) {
      guarantee(errno != 0, "fopen should set errno on error");
      return false;
    }

    _file = file;
    return true;
  }

  bool write_raw(const void *buf, size_t size) {
    assert(_file != nullptr, "file must be opened before writing");
    precond(buf != nullptr || size == 0);
    return size == 0 || fwrite(buf, size, 1, _file) == 1;
  }

  template <class T, ENABLE_IF(std::is_integral<T>::value &&
                               (sizeof(T) == sizeof(u1) || sizeof(T) == sizeof(u2) ||
                                sizeof(T) == sizeof(u4) || sizeof(T) == sizeof(u8)))>
  bool write(T value) {
    T tmp;
    switch (sizeof(value)) {
      case sizeof(u1): tmp = value; break;
      case sizeof(u2): Bytes::put_Java_u2(reinterpret_cast<address>(&tmp), value); break;
      case sizeof(u4): Bytes::put_Java_u4(reinterpret_cast<address>(&tmp), value); break;
      case sizeof(u8): Bytes::put_Java_u8(reinterpret_cast<address>(&tmp), value); break;
    }
    return write_raw(&tmp, sizeof(tmp));
  }

 private:
  FILE *_file = nullptr;
};

class StackDumpWriter : public StackObj {
 private:
  BinaryFileWriter *_writer;

#define WRITE(value) do { if (!_writer->write(value))                     return false; } while (false)
#define WRITE_RAW(value, size) do { if (!_writer->write_raw(value, size)) return false; } while (false)
#define WRITE_CASTED(t, value) do { if (!_writer->write<t>(value))        return false; } while (false)

 public:
  explicit StackDumpWriter(BinaryFileWriter *writer) : _writer(writer) {}

  bool write_header() {
    constexpr char HEADER[] = "JAVA STACK DUMP 0.1";
    WRITE_RAW(HEADER, sizeof(HEADER));

    STATIC_ASSERT(sizeof(uintptr_t) == sizeof(u4) || sizeof(uintptr_t) == sizeof(u8));
    WRITE_CASTED(u2, sizeof(uintptr_t)); // Word size

    return true;
  }

  bool write_stack(const JavaThread *thread, const GrowableArrayView<javaVFrame *> &frames) {
    log_trace(stackdump)("Stack for thread " UINTX_FORMAT " - %s",
                         cast_from_oop<uintptr_t>(thread->threadObj()), thread->name());
    WRITE(oop2uint(thread->threadObj())); // Thread ID

    // Whether the current bytecode in the youngest frame is to be re-executed
    if (frames.length() == 0) {
      log_trace(stackdump)("Re-exec youngest: false (empty trace)");
      WRITE_CASTED(u1, false);
    } else if (frames.first()->is_interpreted_frame()) {
      // TODO is it true that we always want to re-execute?
      log_trace(stackdump)("Re-exec youngest: true (interpreted frame)");
      WRITE_CASTED(u1, true);
    } else {
      // TODO investigate whether the exec_mode used by deoptimization to decide
      //  on re-execution is also required for us here
      bool should_reexecute = compiledVFrame::cast(frames.first())->should_reexecute();
      log_trace(stackdump)("Re-exec youngest: %s (should_reexecute of compiled frame)", BOOL_TO_STR(should_reexecute));
      WRITE_CASTED(u1, should_reexecute);
    }

    log_trace(stackdump)("%i frames", frames.length());
    WRITE_CASTED(u4, frames.length()); // Number of frames in the stack

    for (const javaVFrame *frame : frames) {
      if (log_is_enabled(Trace, stackdump)) {
        if (frame->is_interpreted_frame()) {
          log_trace(stackdump)("== Interpreted frame ==");
        } else {
          precond(frame->is_compiled_frame());
          log_trace(stackdump)("==  Compiled frame   ==");
          // TODO use Deoptimization::realloc_objects(...) to rematerialize
          //  scalar-replaced objects
        }
      }

      if (!write_method(*frame)) {
        return false;
      }

      guarantee(frame->bci() <= UINT16_MAX, "Guaranteed by JVMS §4.7.3 (code_length max value)");
      log_trace(stackdump)("BCI: %i", frame->bci());
      WRITE_CASTED(u2, frame->bci());

      log_trace(stackdump)("Locals:");
      if (!write_stack_values(*frame->locals())) {
        return false;
      }

      log_trace(stackdump)("Operands:");
      if (!write_stack_values(*frame->expressions())) {
        return false;
      }

      log_trace(stackdump)("Monitors: not implemented");
      WRITE_CASTED(u2, 0);

      log_trace(stackdump)("=======================");
    }

    return true;
  }

 private:
  static uintptr_t oop2uint(oop o) {
    STATIC_ASSERT(sizeof(uintptr_t) == sizeof(intptr_t)); // Primitive stack slots
    STATIC_ASSERT(sizeof(uintptr_t) == oopSize);          // IDs
    return cast_from_oop<uintptr_t>(o);
  }

  bool write_method(const javaVFrame &frame) {
    Method *method = frame.method();

    Symbol *name = method->name();
    if (log_is_enabled(Trace, stackdump)) {
      ResourceMark rm;
      log_trace(stackdump)("Method name: " UINTX_FORMAT " - %s",  reinterpret_cast<uintptr_t>(name), name->as_C_string());
    }
    WRITE(reinterpret_cast<uintptr_t>(name));

    Symbol *signature = method->signature();
    if (log_is_enabled(Trace, stackdump)) {
      ResourceMark rm;
      log_trace(stackdump)("Method signature: " UINTX_FORMAT " - %s", reinterpret_cast<uintptr_t>(signature), signature->as_C_string());
    }
    WRITE(reinterpret_cast<uintptr_t>(signature));

    InstanceKlass *holder = method->method_holder();
    if (log_is_enabled(Trace, stackdump)) {
      ResourceMark rm;
      log_trace(stackdump)("Class: " UINTX_FORMAT " - %s", cast_from_oop<uintptr_t>(holder->java_mirror()), holder->external_name());
    }
    WRITE(oop2uint(holder->java_mirror()));

    return true;
  }

  bool write_stack_values(const StackValueCollection &values) {
    guarantee(values.size() <= UINT16_MAX, "Guaranteed by JVMS §4.11");
    log_trace(stackdump)("%i values", values.size());
    WRITE_CASTED(u2, values.size());

    for (int i = 0; i < values.size(); i++) {
      const StackValue &value = *values.at(i);
      switch (value.type()) {
        case T_INT:
          log_trace(stackdump)("  %i - primitive: " INTX_FORMAT " (intptr), " INT32_FORMAT " (jint), " UINTX_FORMAT_X " (hex)",
                               i, value.get_intptr(), value.get_jint(), value.get_intptr());
          WRITE_CASTED(u1, DumpedStackValueType::PRIMITIVE);
          WRITE_CASTED(uintptr_t, value.get_intptr()); // Write the whole slot, i.e. 4 or 8 bytes
          break;
        case T_OBJECT:
          log_trace(stackdump)("  %i - oop: " UINTX_FORMAT "%s",
                               i, cast_from_oop<uintptr_t>(value.get_obj()()), value.obj_is_scalar_replaced() ? " (scalar-replaced)" : "");
          guarantee(!value.obj_is_scalar_replaced(), "Scalar-replaced objects should have been rematerialized");
          WRITE_CASTED(u1, DumpedStackValueType::REFERENCE);
          WRITE(oop2uint(value.get_obj()()));
          break;
        case T_CONFLICT: // Compiled frames may contain these
          log_trace(stackdump)("  %i - dead (dumping as 0)", i);
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

StackDumper::Result StackDumper::dump(const char *path, bool overwrite) {
  guarantee(SafepointSynchronize::is_at_safepoint(),
            "Need safepoint so threads won't change their states after we check them");

  BinaryFileWriter file_writer;
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
