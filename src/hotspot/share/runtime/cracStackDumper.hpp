#ifndef SHARE_RUNTIME_CRACSTACKDUMPER_HPP
#define SHARE_RUNTIME_CRACSTACKDUMPER_HPP

#include "memory/allStatic.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/globalDefinitions.hpp"

// Thread stack dumping in the big-endian binary format described below.
//
// Header:
//   u1... -- null-termiminated string "CRAC STACK DUMP 0.1"
//   u2    -- word size in bytes:
//            4 -- IDs and primitives (PRs) are 4 byte, longs and doubles are
//                 split in half into their slot pairs with the most significant
//                 bits placed in the first slot
//            8 -- IDs and primitives (PRs) are 8 byte, longs and doubles are
//                 stored in the second slot of their slot pairs, while the
//                 contents of the first slot are unspecified
//   TODO to get rid of this 32-/64-bit difference in how primitives are stored
//    we need to be able to differentiate between longs/doubles and other
//    primitives when dumping the stack. This data is kinda available for
//    compiled frames (see StackValue::create_stack_value() which creates
//    StackValues for compiled frames), but not for interpreted ones.
// Stack traces:
//   ID -- ID of the Thread object
//   u4 -- number of frames that follow
//   Frames, from youngest to oldest:
//     ID -- ID of the method name String object
//     ID -- ID of the method signature String object
//     u1 -- method kind:
//           0 -- static method
//           1 -- non-static, non-overpass method
//           2 -- overpass method
//     ID -- ID of the Class object of the method's class
//           TODO JVM TI Redefine/RetransformClass support: add method holder's
//                redefinition version to select the right one on restore.
//     u2 -- bytecode index (BCI) of the current bytecode: for the youngest
//           frame this specifies the bytecode to be executed, and for the rest
//           of the frames this specifies the invoke bytecode being executed
//     u2 -- number of locals that follow
//     Locals array:
//       u1    -- type: 0 -- primitive, 1 -- object reference
//       u1... -- value: PR if the type is 0, ID if the type is 1
//     u2 -- number of operands that follow
//     Operand stack, from oldest to youngest:
//       u1    -- type (same as for locals)
//       u1... -- value (same as for locals)
//     u2 -- number of monitors that follow
//     Monitor infos:
//       TODO describe the monitor info format

// Types of dumped locals and operands.
enum DumpedStackValueType : u1 { PRIMITIVE, REFERENCE };

// Dumps Java frames (until the first CallStub) of non-internal Java threads.
// Threads are dumped in the order they were created (oldest first), dumped IDs
// are oops to be compatible with HeapDumper's object IDs.
struct CracStackDumper : public AllStatic {
  class Result {
   public:
    enum class Code {
      OK,              // Success
      IO_ERROR,        // File IO error, static message is in io_error_msg
      NON_JAVA_ON_TOP, // problematic_thread is running native code
      NON_JAVA_IN_MID  // problematic_thread is running Java code but with a native frame somewhere deeper in its stack
    };

    Result() : _code(Code::OK) {}

    Result(Code code, const char *io_error_msg) : _code(Code::OK), _io_error_msg(io_error_msg) {
      assert(code == Code::IO_ERROR && io_error_msg != nullptr, "Use another constructor for this code");
    }

    Result(Code code, JavaThread *problematic_thread) : _code(code), _problematic_thread(problematic_thread) {
      assert(code > Code::IO_ERROR && problematic_thread != nullptr, "Use another constructor for this code");
    }

    Code code()                      const { return _code; }
    // If the code indicates an IO error, holds its description. Null otherwise.
    const char *io_error_msg()       const { return _io_error_msg; }
    // If the code indicates a non-IO error, holds the thread for which stack
    // dump failed. Null otherwise.
    JavaThread *problematic_thread() const { return _problematic_thread; }

   private:
    const Code _code;
    const char *const _io_error_msg = nullptr;
    JavaThread *const _problematic_thread = nullptr;
  };

  // Dumps the stacks into the specified file, possibly overwriting it if the
  // corresponding parameter is set to true.
  //
  // Must be called on safepoint.
  static Result dump(const char *path, bool overwrite = false);
};

#endif // SHARE_RUNTIME_CRACSTACKDUMPER_HPP
