#ifndef SHARE_UTILITIES_STACK_DUMPER_HPP
#define SHARE_UTILITIES_STACK_DUMPER_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

// Thread stack dumping in the big-endian binary format described below.
//
// Header:
//   u1... -- null-termiminated string "JAVA STACK DUMP 0.1"
//   u2    -- ID size in bytes
// Stack traces:
//   ID -- ID of the Thread object
//   u4 -- number of frames that follow
//   Frames, from youngest to oldest:
//     ID -- ID of the method name String object
//     ID -- ID of the method signature String object
//     ID -- ID of the Class object of the method's class
//     u2 -- bytecode index (BCI) of the next bytecode to be executed in the
//           youngest frame or the call bytecode executed last in any other
//           frame
//     u2 -- number of locals that follow
//     Locals array:
//       u1       -- type:
//                   0 == boolean, byte, char, short, int, or float
//                   1 == long or double, stored in two consequtive elements
//                        with the most significant bits in the first element
//                   2 == object
//       u4 or ID -- value: u4 if type is 0 or 1, ID if type is 2
//     u2 -- number of operands that follow
//     Operand stack, from oldest to youngest:
//       u1       -- type (same as for locals)
//       u4 or ID -- value (same as for locals)
//     u2 -- number of monitors that follow
//     Monitor infos:
//       TODO describe the monitor info format

// Types of dumped locals and operands.
enum DumpedStackValueType : u1 {
  // boolean, byte, char, short, int, or float.
  PRIMITIVE,
  // Half of long or double (most significant bits are in the first half).
  PRIMITIVE_HALF,
  // Object reference.
  REFERENCE
};

// Dumps Java frames (until the first CallStub) of non-internal Java threads.
// Dumped IDs are oops to be compatible with HeapDumper's object IDs.
struct StackDumper : public AllStatic {
  // Dumps the stacks to the specified file, possibly overwriting it if the
  // corresponding parameter is set to true, Returns nullptr on success, or a
  // pointer to a static error message otherwise.
  static const char *dump(const char *path, bool overwrite = false);
};

#endif // SHARE_UTILITIES_STACK_DUMPER_HPP
