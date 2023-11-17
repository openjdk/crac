#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/os.hpp"
#include "runtime/stackValue.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vframe.inline.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/stackDumper.hpp"
#include <cstdint>
#include <type_traits>

#ifdef _LP64
#define WORD_UINT_T u8
#else // _LP64
#define WORD_UINT_T u4
#endif // _LP64
STATIC_ASSERT(sizeof(WORD_UINT_T) == sizeof(intptr_t)); // Primitive stack slots
STATIC_ASSERT(sizeof(WORD_UINT_T) == oopSize);          // IDs

static WORD_UINT_T oop2uint(oop o) {
  return cast_from_oop<WORD_UINT_T>(o);
}

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

  template <class T, ENABLE_IF(std::is_same<T, u1>::value || std::is_same<T, u2>::value ||
                               std::is_same<T, u4>::value || std::is_same<T, u8>::value)>
  bool write(T value) {
    T tmp;
    if (std::is_same<T, u1>::value) {
      tmp = value;
    } else if (std::is_same<T, u2>::value) {
      Bytes::put_Java_u2(reinterpret_cast<address>(&tmp), value);
    } else if (std::is_same<T, u4>::value) {
      Bytes::put_Java_u4(reinterpret_cast<address>(&tmp), value);
    } else if (std::is_same<T, u8>::value) {
      Bytes::put_Java_u8(reinterpret_cast<address>(&tmp), value);
    } else {
      ShouldNotReachHere();
    }
    return write_raw(&tmp, sizeof(tmp));
  }

 private:
  FILE *_file = nullptr;
};

class StackDumpWriter : public StackObj {
 public:
  explicit StackDumpWriter(BinaryFileWriter *writer) : _writer(writer) {}

  bool write_dump() {
    if (!write_header()) {
      return false;
    }

    // TODO decide what to do with threads executing native code
    //  (thread->thread_state() == _thread_in_native)
    for (JavaThreadIteratorWithHandle jtiwh; JavaThread *thread = jtiwh.next();) {
      // Not excluding JVMTI agent and AttachListener threads since they may
      // execute user-visible operations
      if (thread->is_exiting() ||
          thread->is_hidden_from_external_view() ||
          thread->is_Compiler_thread() ||
          (strcmp(thread->name(), "Notification Thread") == 0 && java_lang_Thread::threadGroup(thread->threadObj()) == Universe::system_thread_group())) {
        continue;
      }
      if (!write_stack(thread)) {
        return false;
      }
    }

    return true;
  }

 private:
  BinaryFileWriter *_writer;

#define WRITE(value)                          \
  do {                                        \
    if (!_writer->write(value)) return false; \
  } while (false)

#define WRITE_RAW(value, size)                          \
  do {                                                  \
    if (!_writer->write_raw(value, size)) return false; \
  } while (false)

#define WRITE_CASTED(t, value)                   \
  do {                                           \
    if (!_writer->write<t>(value)) return false; \
  } while (false)

  bool write_header() {
    constexpr char HEADER[] = "JAVA STACK DUMP 0.1";
    WRITE_RAW(HEADER, sizeof(HEADER));

    WRITE_CASTED(u2, sizeof(WORD_UINT_T)); // Word size

    return true;
  }

  bool write_stack(JavaThread *thread) {
    log_trace(stackdump)("Stack for thread " UINTX_FORMAT " - %s",
                         cast_from_oop<uintptr_t>(thread->threadObj()), thread->name());
    WRITE(oop2uint(thread->threadObj())); // Thread ID

    ResourceMark rm; // vframes are resource-allocated

    GrowableArray<javaVFrame *> frames;
    for (vframeStream vfs(thread, /* Stop on CallStub */ true); !vfs.at_end(); vfs.next()) {
      frames.push(vfs.asJavaVFrame());
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

      log_trace(stackdump)("=======================");
    }

    return true;
  }

  bool write_method(const javaVFrame &frame) {
    Method *method = frame.method();
    ResourceMark rm;

    Symbol *name = method->name();
    log_trace(stackdump)("Method name: %s", name->as_C_string());
    WRITE_RAW(name->as_C_string(), name->utf8_length());

    Symbol *signature = method->signature();
    log_trace(stackdump)("Method signature: %s", signature->as_C_string());
    WRITE_RAW(signature->as_C_string(), signature->utf8_length());

    log_trace(stackdump)("Class: " UINTX_FORMAT " - %s",
                         cast_from_oop<uintptr_t>(method->method_holder()->java_mirror()), method->method_holder()->external_name());
    WRITE(oop2uint(method->method_holder()->java_mirror()));

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
          WRITE_CASTED(WORD_UINT_T, value.get_intptr()); // Write the whole slot, i.e. 4 or 8 bytes
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
          WRITE_CASTED(WORD_UINT_T, 0);
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
#undef WORD_UINT_T
};

const char *StackDumper::dump(const char *path, bool overwrite) {
  BinaryFileWriter writer;
  if (!writer.open(path, overwrite)) {
    return os::strerror(errno);
  }
  if (!StackDumpWriter(&writer).write_dump()) {
    return "couldn't write into the opened file";
  }
  return nullptr;
}
