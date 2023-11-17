#ifndef SHARE_UTILITIES_STACK_DUMP_PARSER_HPP
#define SHARE_UTILITIES_STACK_DUMP_PARSER_HPP

#include "memory/allocation.hpp"
#include "utilities/extendableArray.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/stackDumper.hpp"

// Parsed stack trace.
class StackTrace : public CHeapObj<mtInternal> {
 public:
  // Assuming dumped ID type fits into 8 bytes. This is checked when parsing.
  using ID = u8;

  struct Frame : public CHeapObj<mtInternal> {
    struct Value {
      DumpedStackValueType type;
      union {
        u4 prim;
        ID obj_id;
      };
    };

    ID method_name_id;                        // ID of method name string
    ID method_sig_id;                         // ID of method signature string
    ID class_id;                              // ID of class containing the method
    u2 bci;                                   // Index of the bytecode being/to be executed
    ExtendableArray<Value, u2> locals = {};   // Local variables
    ExtendableArray<Value, u2> operands = {}; // Operand/expression stack
    // TODO monitors
  };

  StackTrace(ID thread_id, u4 frames_num)
      : _thread_id(thread_id), _frames_num(frames_num), _frames(new Frame[_frames_num]) {}

  ~StackTrace() { delete[] _frames; }

  NONCOPYABLE(StackTrace);

  // ID of the thread whose stack this is.
  ID thread_id() const            { return _thread_id; }
  // Number of frames in the stack.
  u4 frames_num() const           { return _frames_num; }
  // Stack frames from youngest to oldest.
  const Frame &frames(u4 i) const { precond(i < frames_num()); return _frames[i]; }
  Frame &frames(u4 i)             { precond(i < frames_num()); return _frames[i]; }

 private:
  const ID _thread_id;
  const u4 _frames_num;
  Frame *const _frames;
};

class ParsedStackDump : public StackObj {
 public:
  ~ParsedStackDump() {
    for (auto *_stack_trace : _stack_traces) {
      delete _stack_trace;
    }
  }

  // Actual size of IDs in the dump.
  u2 id_size() const                                      { return _id_size; }
  void set_id_size(u2 value)                              { _id_size = value; }
  // Parsed stack traces.
  const GrowableArray<StackTrace *> &stack_traces() const { return _stack_traces; }
  GrowableArray<StackTrace *> &stack_traces()             { return _stack_traces; }

 private:
  u2 _id_size = 0;
  GrowableArray<StackTrace *> _stack_traces;
};

struct StackDumpParser : public AllStatic {
  // Parses the stack dump in path filling the out container. Returns nullptr on
  // success or a pointer to a static error message otherwise.
  //
  // Stack traces and their frames are resource-allocated.
  static const char *parse(const char *path, ParsedStackDump *out);
};

#endif // SHARE_UTILITIES_STACK_DUMP_PARSER_HPP
