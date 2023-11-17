#ifndef SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP
#define SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP

#include "memory/allocation.hpp"
#include "oops/symbolHandle.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/extendableArray.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/resizeableResourceHash.hpp"
#include "utilities/hprofTag.hpp"

// Relevant HPROF records. See HPROF binary format for details.
struct HeapDump : AllStatic {
  // Assuming HPROF ID type fits into 8 bytes. This is checked when parsing.
  using ID = u8;

  enum class Version { UNKNOWN, V101, V102 };

  union BasicValue {
    ID as_object_id;
    jboolean as_boolean;
    jchar as_char;
    jfloat as_float;
    jdouble as_double;
    jbyte as_byte;
    jshort as_short;
    jint as_int;
    jlong as_long;
  };

  struct UTF8 {
    ID id;
    TempNewSymbol sym;
  };

  struct LoadClass {
    u4 serial;
    ID class_id;
    u4 stack_trace_serial;
    ID class_name_id;
  };

  struct ClassDump {
    struct ConstantPoolEntry {
      u2 index;
      u1 type;
      BasicValue value;
    };

    struct Field {
      struct Info {
        ID name_id;
        u1 type;
      } info;
      BasicValue value;
    };

    ID id;
    u4 stack_trace_serial;
    ID super_id;
    ID class_loader_id;
    ID signers_id;
    ID protection_domain_id;

    u4 instance_size;

    ExtendableArray<ConstantPoolEntry, u2> constant_pool = {}; // = {} allows aggregate initialization without missing-field-initializers warnings
    ExtendableArray<Field, u2> static_fields = {};
    ExtendableArray<Field::Info, u2> instance_field_infos = {};
  };

  struct InstanceDump {
    ID id;
    u4 stack_trace_serial;
    ID class_id;
    // Raw binary data: use read_field() to read it in the correct byte order.
    ExtendableArray<u1, u4> fields_data = {};

    // Reads a field from fields data, returns the amount of bytes read, where 0
    // means a error (illegal arguments or the read violates the array bounds).
    u4 read_field(u4 offset, char sig, u4 id_size, BasicValue *out) const;
  };

  struct ObjArrayDump {
    ID id;
    u4 stack_trace_serial;
    ID array_class_id;
    ExtendableArray<ID, u4> elem_ids = {};
  };

  struct PrimArrayDump {
    ID id;
    u4 stack_trace_serial;
    u4 elems_num;
    u1 elem_type;
    // Elements data, already in the correct byte order.
    ExtendableArray<u1, u8> elems_data = {}; // u8 to index 2^32 (u4 holds # of elems) * 8 (max elem size) bytes
  };

  static u1 prim2size(u1 type) {
    switch (type) {
      case HPROF_BOOLEAN: return sizeof(jboolean);
      case HPROF_CHAR:    return sizeof(jchar);
      case HPROF_FLOAT:   return sizeof(jfloat);
      case HPROF_DOUBLE:  return sizeof(jdouble);
      case HPROF_BYTE:    return sizeof(jbyte);
      case HPROF_SHORT:   return sizeof(jshort);
      case HPROF_INT:     return sizeof(jint);
      case HPROF_LONG:    return sizeof(jlong);
      default:            return 0;
    }
  }
};

struct ParsedHeapDump : public StackObj {
  template <class V>
  using RecordTable = ResizeableResourceHashtable<HeapDump::ID, V, AnyObj::C_HEAP>;

  // Actual size of IDs in the dump.
  u4 id_size;

  RecordTable<HeapDump::UTF8>          utf8s            {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::LoadClass>     load_classes     {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::ClassDump>     class_dumps      {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::InstanceDump>  instance_dumps   {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::ObjArrayDump>  obj_array_dumps  {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::PrimArrayDump> prim_array_dumps {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};

 private:
  // Odd primes picked from ResizeableResourceHashtable.cpp
  static const int INITIAL_TABLE_SIZE = 1009;
  static const int MAX_TABLE_SIZE = 1228891;
};

// Parses HPROF heap dump.
struct HeapDumpParser : public AllStatic {
  // Parses the heap dump in path filling the out container. Returns nullptr on
  // success or a pointer to a static error message otherwise. If a error
  // occurs, out may contain unfilled records.
  static const char *parse(const char *path, ParsedHeapDump *out);
};

#endif // SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP
