#ifndef SHARE_RUNTIME_CRACCLASSDUMPPARSER_HPP
#define SHARE_RUNTIME_CRACCLASSDUMPPARSER_HPP

#include "memory/allocation.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/handles.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/methodKind.hpp"
#include "utilities/pair.hpp"

// Parsed class info that cannot be applied when parsing the dump.
struct UnfilledClassInfo {
  InstanceKlass::ClassState target_state;     // State of the class at dump time
  HeapDump::ID class_initialization_error_id; // Exception object ID
};

// Convenience BasicTypeReader wrapper.
class ClassDumpReader {
 private:
  BasicTypeReader *const _reader;
  u2 _id_size;

 protected:
  explicit ClassDumpReader(BasicTypeReader *reader, u2 id_size = 0) : _reader(reader), _id_size(id_size) {
    assert(_id_size == 0 /* unset */ || is_supported_id_size(id_size), "unsupported ID size");
  };

  static constexpr bool is_supported_id_size(u2 size) {
    return size == sizeof(u8) || size == sizeof(u4) || size == sizeof(u2) || size == sizeof(u1);
  }

  BasicTypeReader *reader() { return _reader; }
  u2 id_size() const        { precond(_id_size > 0); return _id_size; }
  void set_id_size(u2 value, TRAPS);

  void read_raw(void *buf, size_t size, TRAPS);
  template <class T> T read(TRAPS);
  bool read_bool(TRAPS);
  HeapDump::ID read_id(bool can_be_null, TRAPS);
  void skip(size_t size, TRAPS);
};

class ClassLoaderProvider;
struct InterclassRefs;

// Parses a CRaC class dump created and restores classes based on it without
// calling their class loaders.
//
// Note: to improve the restoration performance it is assumed that the dump
// comes from a trusted source and thus only basic correctness checks are
// performed (and the VM will die if those fail).
class CracClassDumpParser: public ClassDumpReader {
 public:
  static void parse(const char *path, const ParsedHeapDump &heap_dump, ClassLoaderProvider *loader_provider,
                    HeapDumpTable<InstanceKlass *,   AnyObj::C_HEAP> *created_iks,
                    HeapDumpTable<ArrayKlass *,      AnyObj::C_HEAP> *created_aks,
                    HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> *unfilled_infos, TRAPS);

  // Finds a normal or a non-generic signature polymorphic method.
  // TODO find a better place for this
  static Method *find_method(InstanceKlass *holder,
                             Symbol *name, Symbol *signature, MethodKind::Enum kind,
                             bool lookup_signature_polymorphic, TRAPS);

 private:
  const ParsedHeapDump &_heap_dump;
  ClassLoaderProvider *const _loader_provider;

  // Not resource-allocated because that would limit parser's usage of resource area
  HeapDumpTable<InstanceKlass *,   AnyObj::C_HEAP> *const _iks;
  HeapDumpTable<ArrayKlass *,      AnyObj::C_HEAP> *const _aks;
  HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> *const _unfilled_infos;

  CracClassDumpParser(BasicTypeReader *reader, const ParsedHeapDump &heap_dump, ClassLoaderProvider *loader_provider,
                      HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> *iks, HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> *aks,
                      HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> *unfilled_infos, TRAPS);

  struct ClassPreamble;

  void parse_header(TRAPS);
  void parse_obj_array_classes(Klass *bottom_class, TRAPS);
  void parse_primitive_array_classes(TRAPS);

  Handle get_class_loader(HeapDump::ID loader_id, TRAPS);

  ClassPreamble parse_instance_class_preamble(TRAPS);
  InstanceKlass *parse_and_define_instance_class(const HeapDump::ClassDump &class_dump, ClassLoaderData *loader_data, InterclassRefs *refs_out, TRAPS);
  GrowableArray<Pair<HeapDump::ID, InterclassRefs>> parse_instance_and_obj_array_classes(TRAPS);

  void parse_initiating_loaders(TRAPS);
};

#endif // SHARE_RUNTIME_CRACCLASSDUMPPARSER_HPP
