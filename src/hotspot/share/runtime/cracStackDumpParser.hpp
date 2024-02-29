#ifndef SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP
#define SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP

#include "memory/allocation.hpp"
#include "oops/instanceKlass.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/extendableArray.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "runtime/cracStackDumper.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/methodKind.hpp"

// Format for stack dump IDs.
#define SDID_FORMAT UINT64_FORMAT

// Parsed stack trace.
class StackTrace : public CHeapObj<mtInternal> {
 public:
  using ID = u8; // ID type always fits into 8 bytes

  class Frame : public CHeapObj<mtInternal> {
   public:
    struct Value {
      DumpedStackValueType type;
      union {
        u8 prim; // Primitive stack slots are either 4 or 8 bytes
        ID obj_id;
      };
    };

    Method *resolve_method(const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &classes,
                           const ParsedHeapDump::RecordTable<HeapDump::UTF8> &symbols, TRAPS);
    Method *method() const { assert(_resolved_method != nullptr, "unresolved"); return _resolved_method; }

    // ID of method name string.
    ID method_name_id() const { return _method_name_id; };
    void set_method_name_id(ID id) { _method_name_id = id; }
    // ID of method signature string.
    ID method_sig_id() const { return _method_sig_id; };
    void set_method_sig_id(ID id) { _method_sig_id = id; }
    // Kind of the method.
    MethodKind::Enum method_kind() const { return _method_kind; };
    void set_method_kind(MethodKind::Enum kind) { _method_kind = kind; }
    // ID of the class containing the method.
    ID method_holder_id() const { return _method_holder_id; };
    void set_method_holder_id(ID id) { _method_holder_id = id; }

    // Index of the current bytecode
    u2 bci() const { return _bci; }
    void set_bci(u2 bci) { _bci = bci;  }

    // Local variables
    const ExtendableArray<Value, u2> &locals() const { return _locals; }
    ExtendableArray<Value, u2> &locals() { return _locals; }

    // Operand/expression stack
    const ExtendableArray<Value, u2> &operands() const { return _operands; }
    ExtendableArray<Value, u2> &operands() { return _operands; }

   private:
    ID _method_name_id;
    ID _method_sig_id;
    MethodKind::Enum _method_kind;
    ID _method_holder_id;
    Method *_resolved_method = nullptr;

    u2 _bci;

    ExtendableArray<Value, u2> _locals;
    ExtendableArray<Value, u2> _operands;
  };

  StackTrace(ID thread_id, u4 frames_num)
      : _thread_id(thread_id),  _frames_num(frames_num), _frames(new Frame[_frames_num]) {}

  ~StackTrace() { delete[] _frames; }

  // ID of the thread whose stack this is.
  ID thread_id() const           { return _thread_id; }
  // Number of frames in the stack.
  u4 frames_num() const          { return _frames_num; }
  // Stack frames from youngest to oldest.
  const Frame &frame(u4 i) const { precond(i < frames_num()); return _frames[i]; }
  Frame &frame(u4 i)             { precond(i < frames_num()); return _frames[i]; }

 private:
  const ID _thread_id;
  const u4 _frames_num;
  Frame *const _frames;
};

class ParsedStackDump : public CHeapObj<mtInternal> {
 public:
  ~ParsedStackDump() {
    for (auto *_stack_trace : _stack_traces) {
      delete _stack_trace;
    }
  }

  // Size of IDs and stack slots in the dump.
  u2 word_size() const                                         { return _word_size; }
  void set_word_size(u2 value)                                 { _word_size = value; }
  // Parsed stack traces.
  const GrowableArrayView<StackTrace *> &stack_traces() const  { return _stack_traces; }
  GrowableArrayCHeap<StackTrace *, mtInternal> &stack_traces() { return _stack_traces; }

 private:
  u2 _word_size = 0;
  GrowableArrayCHeap<StackTrace *, mtInternal> _stack_traces;
};

struct CracStackDumpParser : public AllStatic {
  // Parses the stack dump in path filling the out container. Returns nullptr on
  // success or a pointer to a static error message otherwise.
  static const char *parse(const char *path, ParsedStackDump *out);
};

#endif // SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP
