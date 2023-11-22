#ifndef SHARE_UTILITIES_STACK_DUMPER_HPP
#define SHARE_UTILITIES_STACK_DUMPER_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

// Thread stack dumping in the big-endian binary format described below.
//
// Header:
//   u1... -- null-termiminated string "JAVA STACK DUMP 0.1"
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
//   u1 -- meaning of the bytecode index (BCI) for the youngest frame:
//         0 -- either the BCI of the youngest frame specifies a bytecode which
//              has been executed or there are no frames in the trace
//         1 -- the BCI of the youngest frame specifies a bytecode to be
//              executed next
//   u4 -- number of frames that follow
//   Frames, from youngest to oldest:
//     ID -- ID of the method name String object
//     ID -- ID of the method signature String object
//     ID -- ID of the Class object of the method's class
//     u2 -- bytecode index (BCI) of the current bytecode: for the youngest
//           frame see the BCI meaning in the trace preamble, and for the rest
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
// Dumped IDs are oops to be compatible with HeapDumper's object IDs.
struct StackDumper : public AllStatic {
  // Dumps the stacks to the specified file, possibly overwriting it if the
  // corresponding parameter is set to true, Returns nullptr on success, or a
  // pointer to a static error message otherwise.
  static const char *dump(const char *path, bool overwrite = false);
};

#endif // SHARE_UTILITIES_STACK_DUMPER_HPP
