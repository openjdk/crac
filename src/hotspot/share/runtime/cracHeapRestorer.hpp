#ifndef SHARE_RUNTIME_CRACHEAPRESTORER_HPP
#define SHARE_RUNTIME_CRACHEAPRESTORER_HPP

#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/handles.hpp"
#include "runtime/reflectionUtils.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpClasses.hpp"
#include "utilities/heapDumpParser.hpp"

// Matches objects important to the VM with their IDs in the dump.
class WellKnownObjects {
 public:
  explicit WellKnownObjects(const ParsedHeapDump &heap_dump, TRAPS) {
    find_well_known_class_loaders(heap_dump, CHECK);
    // TODO other well-known objects (from Universe, security manager etc.)
  }

  // Adds the collected well-known objects into the table. Should be called
  // before restoring any objects to avoid re-creating the existing well-known
  // objects.
  void put_into(HeapDumpTable<Handle, AnyObj::C_HEAP> *objects) const;

  // Sets the well-known objects that are not yet set in this VM and checks
  // that the ones that are set have the specified values.
  void get_from(const HeapDumpTable<Handle, AnyObj::C_HEAP> &objects) const;

 private:
  HeapDump::ID _platform_loader_id = HeapDump::NULL_ID;       // Built-in platform loader
  HeapDump::ID _builtin_system_loader_id = HeapDump::NULL_ID; // Built-in system loader
  HeapDump::ID _actual_system_loader_id = HeapDump::NULL_ID;  // Either the built-in system loader or a user-provided one

  void find_well_known_class_loaders(const ParsedHeapDump &heap_dump, TRAPS);

  void lookup_builtin_class_loaders(const ParsedHeapDump &heap_dump,
                                    const HeapDump::LoadClass &jdk_internal_loader_ClassLoaders);

  void lookup_actual_system_class_loader(const ParsedHeapDump &heap_dump,
                                         const HeapDump::LoadClass &java_lang_ClassLoader);
};

// Interface for providing partially restored ClassLoaders for class definition.
class ClassLoaderProvider : public StackObj {
 public:
  // Returns a ClassLoader object with the requested ID.
  //
  // If the object has previously been allocated the same object is returned.
  // Otherwise, the object is allocated.
  virtual instanceHandle get_class_loader(HeapDump::ID id, TRAPS) = 0;
};

struct UnfilledClassInfo;
class CracStackTrace;

// Restores heap based on an HPROF dump created by HeapDumper (there are some
// assumptions that are not guaranteed by the general HPROF standard).
class CracHeapRestorer : public ClassLoaderProvider {
 public:
  // Allocates resources, caller must set a resource mark.
  CracHeapRestorer(const ParsedHeapDump &heap_dump,
                   const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &instance_classes,
                   const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &array_classes,
                   TRAPS) :
      _heap_dump(heap_dump), _instance_classes(instance_classes), _array_classes(array_classes),
      _well_known_objects(heap_dump, THREAD) {
    if (HAS_PENDING_EXCEPTION) {
      return;
    }
    _well_known_objects.put_into(&_objects);
  };

  instanceHandle get_class_loader(HeapDump::ID id, TRAPS) override;

  void restore_heap(const HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> &class_infos,
                    const GrowableArrayView<CracStackTrace *> &stack_traces, TRAPS);

 private:
  const ParsedHeapDump &_heap_dump;
  const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &_instance_classes;
  const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &_array_classes;

  const WellKnownObjects _well_known_objects;

  // Not resource-allocated because that would limit resource usage between
  // getting class loaders and restoring the heap
  HeapDumpTable<Handle, AnyObj::C_HEAP> _objects{1009, 100000};
  HeapDumpTable<bool, AnyObj::C_HEAP> _prepared_loaders{3, 127};

  HeapDumpClasses::java_lang_ClassLoader _loader_dump_reader;
  HeapDumpClasses::java_lang_Class _mirror_dump_reader;
  HeapDumpClasses::java_lang_String _string_dump_reader;
  HeapDumpClasses::java_lang_invoke_ResolvedMethodName _resolved_method_name_dump_reader;
  HeapDumpClasses::java_lang_invoke_MemberName _member_name_dump_reader;

  InstanceKlass &get_instance_class(HeapDump::ID id) const;
  ArrayKlass &get_array_class(HeapDump::ID id) const;

  Handle get_object_when_present(HeapDump::ID id) const; // Always not null (nulls aren't recorded)
  Handle get_object_if_present(HeapDump::ID id) const;   // Null iff the object has not been recorded yet
  void put_object_when_absent(HeapDump::ID id, Handle obj);
  void put_object_if_absent(HeapDump::ID id, Handle obj);

  // Partially restores the class loader so it can be used for class definition.
  instanceHandle prepare_class_loader(HeapDump::ID id, TRAPS);
  instanceHandle get_class_loader_parent(const HeapDump::InstanceDump &loader_dump, TRAPS);
  instanceHandle get_class_loader_name(const HeapDump::InstanceDump &loader_dump, bool with_id, TRAPS);
  instanceHandle get_class_loader_unnamed_module(const HeapDump::InstanceDump &loader_dump, TRAPS);
  instanceHandle get_class_loader_parallel_lock_map(const HeapDump::InstanceDump &loader_dump, TRAPS);

  void find_and_record_class_mirror(const HeapDump::ClassDump &class_dump, TRAPS);
  void record_class_mirror(instanceHandle mirror, const HeapDump::InstanceDump &mirror_dump, TRAPS);

  instanceHandle intern_if_needed(instanceHandle string, const HeapDump::InstanceDump &dump, TRAPS);
  methodHandle get_resolved_method(const HeapDump::InstanceDump &resolved_method_name_dump, TRAPS);

  void set_field(instanceHandle obj, const FieldStream &fs, const HeapDump::BasicValue &val, TRAPS);
#define set_instance_field_if_special_signature(name) \
  bool name(instanceHandle, const HeapDump::InstanceDump &, const FieldStream &, const DumpedInstanceFieldStream &, TRAPS);
  using set_instance_field_if_special_ptr_t = set_instance_field_if_special_signature((CracHeapRestorer::*));
  set_instance_field_if_special_signature(set_class_loader_instance_field_if_special);
  set_instance_field_if_special_signature(set_class_mirror_instance_field_if_special);
  set_instance_field_if_special_signature(set_string_instance_field_if_special);
  set_instance_field_if_special_signature(set_member_name_instance_field_if_special);
  set_instance_field_if_special_signature(set_call_site_instance_field_if_special);
#undef set_instance_field_if_special_signature
  void restore_special_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                       set_instance_field_if_special_ptr_t set_field_if_special, TRAPS);
  void restore_ordinary_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump, TRAPS);
  void restore_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump, TRAPS);
  static bool set_static_field_if_special(instanceHandle mirror, const FieldStream &fs, const HeapDump::BasicValue &val);
  void restore_static_fields(InstanceKlass *ik, const HeapDump::ClassDump &dump, TRAPS);

  void restore_class_mirror(HeapDump::ID id, TRAPS);
  Handle restore_object(HeapDump::ID id, TRAPS);
  instanceHandle restore_instance(const HeapDump::InstanceDump &dump, TRAPS);
  objArrayHandle restore_obj_array(const HeapDump::ObjArrayDump &dump, TRAPS);
  typeArrayHandle restore_prim_array(const HeapDump::PrimArrayDump &dump, TRAPS);
};

#endif // SHARE_RUNTIME_CRACHEAPRESTORER_HPP
