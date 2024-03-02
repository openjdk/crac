#ifndef SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP
#define SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP

#include "jni.h"
#include "memory/allocation.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/handles.hpp"
#include "runtime/jniHandles.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/methodKind.hpp"
#include <type_traits>

// Note: stack info parsing and usage happen in different resource and handle
// scopes -- that is why everything here is heap-allocated and JNI handles are
// used for OOPs

// Format for stack dump IDs.
#define SDID_FORMAT UINT64_FORMAT

// Parsed stack trace.
class CracStackTrace : public CHeapObj<mtInternal> {
 public:
  using ID = u8; // ID type always fits into 8 bytes

  class Frame : public CHeapObj<mtInternal> {
   public:
    class Value {
     public:
      enum class Type : u1 {
        EMPTY, // Unfilled
        PRIM,  // Primitive stack value
        REF,   // Unresolved reference ID
        OBJ    // Resolved reference (JNI handle owned by this value)
      };

      Value() = default;
      inline static Value of_primitive(u8 val) { return {Type::PRIM, val}; }
      inline static Value of_obj_id(ID id)     { return {Type::REF, id}; }
      inline static Value of_obj(Handle obj)   { return {JNIHandles::make_global(obj)}; }

      // Copying: creates a new JNI handle if resolved.
      Value(const Value &other);
      Value &operator=(Value other);

      ~Value() {
        if (type() == Type::OBJ) {
          JNIHandles::destroy_global(as_obj());
        }
      }

      inline Type type() const       { return _type; }

      inline u8 as_primitive() const { precond(type() == Type::PRIM); return _prim; }
      inline ID as_obj_id() const    { precond(type() == Type::REF);  return _obj_id; }
      inline jobject as_obj() const  { precond(type() == Type::OBJ);  return _obj; }

     private:
      Type _type = Type::EMPTY;
      union {
        u8 _prim;     // If the stack slot is 4 bytes only the low half of u8 is used
        ID _obj_id;
        jobject _obj; // JNI handle
      };

      Value(jobject obj) : _type(Type::OBJ), _obj(obj) {} // Implicit for no-copy return in of_obj()
      Value(Type type, u8 value) : _type(type), _prim(value) {
        STATIC_ASSERT((std::is_same<decltype(_prim), decltype(_obj_id)>::value));
        assert(type == Type::PRIM || type == Type::REF, "use the other constructors");
      }
    };

    Method *resolve_method(const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &classes,
                           const ParsedHeapDump::RecordTable<HeapDump::UTF8> &symbols, TRAPS);
    Method *method() const { assert(_resolved_method != nullptr, "unresolved"); return _resolved_method; }

    ID method_name_id() const                   { return _method_name_id; };
    void set_method_name_id(ID id)              { _method_name_id = id; }

    ID method_sig_id() const                    { return _method_sig_id; };
    void set_method_sig_id(ID id)               { _method_sig_id = id; }

    MethodKind::Enum method_kind() const        { return _method_kind; };
    void set_method_kind(MethodKind::Enum kind) { _method_kind = kind; }

    ID method_holder_id() const                 { return _method_holder_id; };
    void set_method_holder_id(ID id)            { _method_holder_id = id; }

    u2 bci() const                              { return _bci; }
    void set_bci(u2 bci)                        { _bci = bci;  }

    const GrowableArrayCHeap<Value, mtInternal> &locals() const   { return _locals; }
    GrowableArrayCHeap<Value, mtInternal> &locals()               { return _locals; }

    const GrowableArrayCHeap<Value, mtInternal> &operands() const { return _operands; }
    GrowableArrayCHeap<Value, mtInternal> &operands()             { return _operands; }

   private:
    ID _method_name_id;
    ID _method_sig_id;
    MethodKind::Enum _method_kind;
    ID _method_holder_id;
    Method *_resolved_method = nullptr;

    u2 _bci;

    GrowableArrayCHeap<Value, mtInternal> _locals;
    GrowableArrayCHeap<Value, mtInternal> _operands;
  };

  CracStackTrace(ID thread_id, u4 frames_num)
      : _thread_id(thread_id),  _frames_num(frames_num), _frames(new Frame[_frames_num]) {}

  NONCOPYABLE(CracStackTrace);

  ~CracStackTrace() { delete[] _frames; }

  // ID of the thread whose stack this is.
  ID thread_id() const           { return _thread_id; }

  // Number of frames in the stack.
  u4 frames_num() const          { return _frames_num; }
  // Frames from youngest to oldest.
  const Frame &frame(u4 i) const { precond(i < frames_num()); return _frames[i]; }
  Frame &frame(u4 i)             { precond(i < frames_num()); return _frames[i]; }

 private:
  const ID _thread_id;
  const u4 _frames_num;
  Frame *const _frames;
};

class ParsedCracStackDump : public CHeapObj<mtInternal> {
 public:
  ~ParsedCracStackDump() {
    for (auto *_stack_trace : _stack_traces) {
      delete _stack_trace;
    }
  }

  // Size of IDs and stack slots in the dump.
  u2 word_size() const                                         { return _word_size; }
  void set_word_size(u2 value)                                 { _word_size = value; }
  // Parsed stack traces.
  const GrowableArrayView<CracStackTrace *> &stack_traces() const  { return _stack_traces; }
  GrowableArrayCHeap<CracStackTrace *, mtInternal> &stack_traces() { return _stack_traces; }

 private:
  u2 _word_size = 0;
  GrowableArrayCHeap<CracStackTrace *, mtInternal> _stack_traces;
};

struct CracStackDumpParser : public AllStatic {
  // Parses the stack dump in path filling the out container. Returns nullptr on
  // success or a pointer to a static error message otherwise.
  static const char *parse(const char *path, ParsedCracStackDump *out);
};

#endif // SHARE_RUNTIME_CRACSTACKDUMPPARSER_HPP
