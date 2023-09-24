#ifndef SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP
#define SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP

#include "memory/allocation.hpp"
#include "oops/symbolHandle.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/resizeableResourceHash.hpp"
#include "utilities/hprofTag.hpp"

// Relevant HPROF records. See HPROF binary format for details.
struct HeapDumpFormat : AllStatic {
  // Assuming HPROF ID type fits into 8 bytes. This is checked when parsing.
  using id_t = u8;

  // Simple array-with-destructor implementation.
  template <class ElemT, class SizeT>
  class Array : public StackObj {
   public:
    explicit Array(SizeT size) : _size(size) {
      precond(size >= 0);
      if (size == 0) {
        return;
      }
      _mem = static_cast<ElemT *>(os::malloc(size * sizeof(ElemT), mtInternal));
      if (mem() == nullptr) {
        vm_exit_out_of_memory(size * sizeof(ElemT), OOM_MALLOC_ERROR,
                              "construction of a heap dump parsing array");
      }
    }

    Array() : Array(0){};

    ~Array() { os::free(mem()); }

    NONCOPYABLE(Array);

    void extend_to(SizeT new_size) {
      precond(new_size >= size());
      if (new_size == size()) {
        return;
      }
      _mem = static_cast<ElemT *>(os::realloc(mem(), new_size * sizeof(ElemT), mtInternal));
      if (mem() == nullptr) {
        vm_exit_out_of_memory(new_size * sizeof(ElemT), OOM_MALLOC_ERROR,
                              "extension of a heap dump parsing array");
      }
      _size = new_size;
    }

    SizeT size() const { return _size; }
    ElemT *mem() const { return _mem; }

    ElemT &operator[](SizeT index) {
      precond(0 <= index && index < _size);
      precond(_mem != nullptr);
      return _mem[index];
    }

    const ElemT &operator[](SizeT index) const {
      precond(0 <= index && index < _size);
      precond(_mem != nullptr);
      return _mem[index];
    }

   private:
    SizeT _size;
    ElemT *_mem;
  };

  enum class Version { UNKNOWN, V101, V102 };

  union BasicValue {
    id_t as_object_id;
    jboolean as_boolean;
    jchar as_char;
    jfloat as_float;
    jdouble as_double;
    jbyte as_byte;
    jshort as_short;
    jint as_int;
    jlong as_long;
  };

  struct UTF8Record {
    id_t id;
    TempNewSymbol sym;
  };

  struct LoadClassRecord {
    u4 serial;
    id_t class_id;
    u4 stack_trace_serial;
    id_t class_name_id;
  };

  struct ClassDumpRecord {
    struct ConstantPoolEntry {
      u2 index;
      u1 type;
      BasicValue value;
    };

    struct Field {
      struct Info {
        id_t name_id;
        u1 type;
      } info;
      BasicValue value;
    };

    id_t id;
    u4 stack_trace_serial;
    id_t super_id;
    id_t class_loader_id;
    id_t signers_id;
    id_t protection_domain_id;

    u4 instance_size;

    Array<ConstantPoolEntry, u2> constant_pool;
    Array<Field, u2> static_fields;
    Array<Field::Info, u2> instance_field_infos;
  };

  struct InstanceDumpRecord {
    id_t id;
    u4 stack_trace_serial;
    id_t class_id;
    // Raw binary data: use read_field() to read it in the correct byte order.
    Array<u1, u4> fields_data;

    // Reads a field from fields data, returns the amount of bytes read where 0
    // means a error (illegal arguments or the read violates the array bounds).
    size_t read_field(u4 offset, char sig, u4 id_size, BasicValue *out) const;
  };

  struct ObjArrayDumpRecord {
    id_t id;
    u4 stack_trace_serial;
    id_t array_class_id;
    Array<id_t, u4> elem_ids;
  };

  struct PrimArrayDumpRecord {
    id_t id;
    u4 stack_trace_serial;
    u4 elems_num;
    u1 elem_type;
    // Elements data, already in the correct byte order.
    Array<u1, u4> elems_data;
  };

  static size_t prim2size(u1 type) {
    switch (type) {
      case HPROF_BOOLEAN:       return sizeof(jboolean);
      case HPROF_CHAR:          return sizeof(jchar);
      case HPROF_FLOAT:         return sizeof(jfloat);
      case HPROF_DOUBLE:        return sizeof(jdouble);
      case HPROF_BYTE:          return sizeof(jbyte);
      case HPROF_SHORT:         return sizeof(jshort);
      case HPROF_INT:           return sizeof(jint);
      case HPROF_LONG:          return sizeof(jlong);
      default:                  return 0;
    }
  }
};

struct ParsedHeapDump : public StackObj {
  using hdf = HeapDumpFormat;
  template <class V>
  using RecordHashtable = ResizeableResourceHashtable<hdf::id_t, V, AnyObj::C_HEAP>;

  // TODO remove after instance field parsing is implemented inside the parser
  u4 id_size;

  RecordHashtable<hdf::UTF8Record>          utf8_records            {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordHashtable<hdf::LoadClassRecord>     load_class_records      {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordHashtable<hdf::ClassDumpRecord>     class_dump_records      {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordHashtable<hdf::InstanceDumpRecord>  instance_dump_records   {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordHashtable<hdf::ObjArrayDumpRecord>  obj_array_dump_records  {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};
  RecordHashtable<hdf::PrimArrayDumpRecord> prim_array_dump_records {INITIAL_TABLE_SIZE, MAX_TABLE_SIZE};

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

#endif  // SHARE_UTILITIES_HEAP_DUMP_PARSER_HPP
