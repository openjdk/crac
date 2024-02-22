#ifndef SHARE_UTILITIES_HEAPDUMPPARSER_HPP
#define SHARE_UTILITIES_HEAPDUMPPARSER_HPP

#include "memory/allocation.hpp"
#include "oops/symbolHandle.hpp"
#include "utilities/debug.hpp"
#include "utilities/extendableArray.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/hprofTag.hpp"
#include "utilities/resizeableResourceHash.hpp"

// Format for heap dump IDs.
#define HDID_FORMAT UINT64_FORMAT

// Relevant HPROF records. See HPROF binary format for details.
struct HeapDump : AllStatic {
  // Assuming HPROF ID type fits into 8 bytes. This is checked when parsing.
  using ID = u8;
  // Represents a null object reference (this is a convention and not a part of
  // HPROF specification).
  static constexpr ID NULL_ID = 0;

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

    // Reads a field from fields data. The caller is responsible for providing
    // the right offset and type.
    BasicValue read_field(u4 offset, BasicType type, u4 id_size) const;
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

  static constexpr BasicType htype2btype(u1 hprof_type) {
    switch (hprof_type) {
      case HPROF_BOOLEAN:       return T_BOOLEAN;
      case HPROF_CHAR:          return T_CHAR;
      case HPROF_FLOAT:         return T_FLOAT;
      case HPROF_DOUBLE:        return T_DOUBLE;
      case HPROF_BYTE:          return T_BYTE;
      case HPROF_SHORT:         return T_SHORT;
      case HPROF_INT:           return T_INT;
      case HPROF_LONG:          return T_LONG;
      case HPROF_NORMAL_OBJECT: return T_OBJECT;
      default:                  return T_ILLEGAL; // Includes HPROF_ARRAY_OBJECT which is not used
    }
  }

  static u4 value_size(BasicType btype, u4 id_size) {
    precond(is_java_type(btype));
    return is_java_primitive(btype) ? type2aelembytes(btype) : id_size;
  }
};

template <class V, AnyObj::allocation_type ALLOC_TYPE = AnyObj::RESOURCE_AREA>
using HeapDumpTable = ResizeableResourceHashtable<HeapDump::ID, V, ALLOC_TYPE>;

struct ParsedHeapDump : public CHeapObj<mtInternal> {
  template <class V>
  using RecordTable = HeapDumpTable<V, AnyObj::C_HEAP>; // TODO use resource area

  // Actual size of IDs in the dump.
  u4 id_size;

  RecordTable<HeapDump::UTF8>          utf8s            {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::LoadClass>     load_classes     {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::ClassDump>     class_dumps      {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::InstanceDump>  instance_dumps   {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::ObjArrayDump>  obj_array_dumps  {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordTable<HeapDump::PrimArrayDump> prim_array_dumps {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};

  Symbol *get_symbol(HeapDump::ID id) const {
    const HeapDump::UTF8 *utf8 = utf8s.get(id);
    guarantee(utf8 != nullptr, "UTF-8 record " HDID_FORMAT " is not in the heap dump", id);
    assert(utf8->sym != nullptr, "must be");
    return utf8->sym;
  }

  Symbol *get_class_name(HeapDump::ID class_id) const {
    const HeapDump::LoadClass *lc = load_classes.get(class_id);
    guarantee(lc != nullptr, "LoadClass record " HDID_FORMAT " is not in the heap dump", class_id);
    Symbol *const name = get_symbol(lc->class_name_id);
    return name;
  }

  const HeapDump::ClassDump &get_class_dump(HeapDump::ID id) const {
    HeapDump::ClassDump *const dump = class_dumps.get(id);
    guarantee(dump != nullptr, "ClassDump record " HDID_FORMAT " is not in the heap dump", id);
    return *dump;
  }

  const HeapDump::InstanceDump &get_instance_dump(HeapDump::ID id) const {
    HeapDump::InstanceDump *const dump = instance_dumps.get(id);
    guarantee(dump != nullptr, "InstanceDump record " HDID_FORMAT " is not in the heap dump", id);
    return *dump;
  }

 private:
  // Odd primes picked from ResizeableResourceHashtable.cpp
  static const int INITIAL_TABLE_SIZE = 1009;
  static const int MAX_TABLE_SIZE = 1228891;
};

// Reads field values from an instance dump.
//
// Usage:
//
//    for (DumpedInstanceFieldStream st(heap_dump, inst_dump); !st.eos(); st.next()) {
//      Symbol* field_name = st.name();
//      ...
//    }
class DumpedInstanceFieldStream {
 private:
  const ParsedHeapDump &_heap_dump;
  const HeapDump::InstanceDump &_instance_dump;

  const HeapDump::ClassDump *_current_class_dump;
  u2 _field_index = 0;  // Index in the current class
  u4 _field_offset = 0; // Offset into the instance field data

 public:
  DumpedInstanceFieldStream(const ParsedHeapDump &heap_dump, const HeapDump::InstanceDump &dump) :
    _heap_dump(heap_dump), _instance_dump(dump), _current_class_dump(&heap_dump.get_class_dump(dump.class_id)) {}

  void next();
  bool eos();

  Symbol *name() const;
  BasicType type() const;
  HeapDump::BasicValue value() const;
};

// Parses HPROF heap dump.
struct HeapDumpParser : public AllStatic {
  // Parses the heap dump in path filling the out container. Returns nullptr on
  // success or a pointer to a static error message otherwise. If a error
  // occurs, out may contain unfilled records.
  static const char *parse(const char *path, ParsedHeapDump *out);
};

#endif // SHARE_UTILITIES_HEAPDUMPPARSER_HPP
